"""FastAPI server: serves the dashboard and a small JSON/SSE API around the Engine."""
from __future__ import annotations

import asyncio
import json
import pathlib

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .engine import Engine

UI_DIR = pathlib.Path(__file__).resolve().parents[1] / "ui"


class PositionIn(BaseModel):
    side: str
    amount: float  # dollars you spent
    price: float | None = None  # dollars per share (0.026 = 2.6¢); defaults to the live ask
    note: str = ""


class CloseIn(BaseModel):
    price: float | None = None  # defaults to the live bid


def create_app(engine: Engine) -> FastAPI:
    app = FastAPI(title="WTI 15-min coach", version="0.1")
    app.state.engine = engine
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
                             "positions": engine.store.positions(60)})

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
            },
        })

    @app.post("/api/position")
    async def open_position(body: PositionIn):
        try:
            pos = engine.open_position(body.side, body.amount, body.price, body.note)
        except ValueError as exc:
            raise HTTPException(400, str(exc))
        return pos.as_dict()

    @app.post("/api/position/close")
    async def close_position(body: CloseIn):
        try:
            return engine.close_position(body.price)
        except ValueError as exc:
            raise HTTPException(400, str(exc))

    @app.delete("/api/position")
    async def cancel_position():
        engine.cancel_position()
        return {"ok": True}

    return app
