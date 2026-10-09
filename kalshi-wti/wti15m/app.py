"""FastAPI server: serves the dashboard and a small JSON/SSE API around the Engine."""
from __future__ import annotations

import asyncio
import json
import pathlib

from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.middleware.trustedhost import TrustedHostMiddleware
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .engine import Engine

UI_DIR = pathlib.Path(__file__).resolve().parents[1] / "ui"
OWN_HEADER = "x-wti15m"


LOCAL_HOSTS = {"127.0.0.1", "localhost", "::1", "[::1]"}


async def from_dashboard(request: Request):
    """Every state-changing call must carry a custom header and come from the dashboard's own origin. A web page
    you happen to have open in another tab cannot add the header to a cross-site request without a CORS preflight,
    which this server never answers; a page that rebinds its DNS name to 127.0.0.1 would be same-origin in the
    browser's eyes, so the Host header must be the local one too and the browser's own Sec-Fetch-Site, when it
    sends one, must say same-origin."""
    if request.headers.get(OWN_HEADER) != "1":
        raise HTTPException(403, "missing dashboard header")
    host = (request.headers.get("host") or "").rsplit(":", 1)[0] if not (request.headers.get("host") or "").startswith("[") \
        else (request.headers.get("host") or "").split("]")[0] + "]"
    if host not in LOCAL_HOSTS:
        raise HTTPException(403, "not the local dashboard")
    site = request.headers.get("sec-fetch-site")
    if site is not None and site not in ("same-origin", "none"):
        raise HTTPException(403, "cross-site request refused")


class PositionIn(BaseModel):
    side: str
    amount: float  # dollars you spent
    price: float | None = None  # dollars per share (0.026 = 2.6¢); defaults to the live ask
    note: str = ""
    target: float | None = None  # a sell target you took from the quick-scalps card (dollars)


class CloseIn(BaseModel):
    price: float | None = None  # defaults to the live bid


def create_app(engine: Engine) -> FastAPI:
    app = FastAPI(title="WTI 15-min coach", version="0.1")
    app.state.engine = engine
    # Only the dashboard's own address may talk to this server: a DNS-rebound page carries its own name in Host.
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=sorted(LOCAL_HOSTS | {engine.cfg.server.host}))
    app.mount("/ui", StaticFiles(directory=str(UI_DIR)), name="ui")

    @app.get("/")
    async def index():
        return FileResponse(str(UI_DIR / "index.html"))

    @app.get("/api/state")
    async def state():
        return JSONResponse(engine.state)

    @app.get("/api/stream")
    async def stream():
        async def gen():
            while True:
                yield f"data: {json.dumps(engine.state)}\n\n"
                await asyncio.sleep(1.0)
        return StreamingResponse(gen(), media_type="text/event-stream",
                                 headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

    @app.get("/api/chart")
    async def chart(minutes: float = 20):
        return JSONResponse(engine.chart(minutes))

    @app.get("/api/history")
    async def history(limit: int = 60):
        return JSONResponse({"windows": engine.store.recent_windows(limit), "paper": engine.store.paper_trades(60),
                             "positions": engine.store.positions(60), "plans": engine.store.plans(60)})

    @app.get("/api/stats")
    async def stats():
        return JSONResponse(engine.stats())

    @app.get("/api/rules")
    async def rules():
        m = engine.tracker.current
        return JSONResponse({
            "series": engine.tracker.series.summary() if engine.tracker.series else None,
            "fees": {"fee_type": engine.fees.fee_type, "multiplier": engine.fees.multiplier},
            "market_rules": m.rules_primary if m else None,
            "market_title": m.title if m else None,
            "feed": engine.feed.health().as_dict(),
            "config": {
                "kalshi": vars(engine.cfg.kalshi), "feed": {k: v for k, v in vars(engine.cfg.feed).items() if "key" not in k},
                "trading": vars(engine.cfg.trading), "config_path": engine.cfg.path,
                "auto": {k: v for k, v in vars(engine.cfg.auto).items() if k not in ("api_key_id", "private_key_path")},
            },
        })

    @app.post("/api/auto/pause", dependencies=[Depends(from_dashboard)])
    async def auto_pause():
        if engine.auto is None:
            raise HTTPException(400, "auto trading is not enabled")
        engine.auto.pause("paused by you")
        return {"ok": True}

    @app.post("/api/auto/resume", dependencies=[Depends(from_dashboard)])
    async def auto_resume():
        if engine.auto is None:
            raise HTTPException(400, "auto trading is not enabled")
        engine.auto.resume()
        return {"ok": True}

    @app.post("/api/auto/stop", dependencies=[Depends(from_dashboard)])
    async def auto_stop():
        if engine.auto is None:
            raise HTTPException(400, "auto trading is not enabled")
        await engine.auto.stop_all()
        return {"ok": True}

    @app.get("/api/auto/orders")
    async def auto_orders(limit: int = 100):
        return JSONResponse({"orders": engine.store.orders(limit)})

    @app.post("/api/position", dependencies=[Depends(from_dashboard)])
    async def open_position(body: PositionIn):
        try:
            pos = engine.open_position(body.side, body.amount, body.price, body.note, body.target)
        except ValueError as exc:
            raise HTTPException(400, str(exc))
        return pos.as_dict()

    @app.post("/api/position/close", dependencies=[Depends(from_dashboard)])
    async def close_position(body: CloseIn):
        try:
            return engine.close_position(body.price)
        except ValueError as exc:
            raise HTTPException(400, str(exc))

    @app.delete("/api/position", dependencies=[Depends(from_dashboard)])
    async def cancel_position():
        engine.cancel_position()
        return {"ok": True}

    return app
