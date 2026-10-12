"""FastAPI server: the board's landing page, one SSE stream for every card, and a detail endpoint per market."""
from __future__ import annotations

import asyncio
import json
import pathlib

from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.middleware.trustedhost import TrustedHostMiddleware
from fastapi.responses import HTMLResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import __version__
from .board import Board

UI_DIR = pathlib.Path(__file__).resolve().parents[1] / "ui"
OWN_HEADER = "x-cm15m"
LOCAL_HOSTS = {"127.0.0.1", "localhost", "::1", "[::1]"}


async def from_dashboard(request: Request):
    """State-changing calls must carry the board's own header and come from the board's own local origin (a page in
    another tab cannot add the header cross-site without a CORS preflight, which this server never answers)."""
    if request.headers.get(OWN_HEADER) != "1":
        raise HTTPException(403, "missing dashboard header")
    raw = request.headers.get("host") or ""
    host = raw.split("]")[0] + "]" if raw.startswith("[") else raw.rsplit(":", 1)[0]
    if host not in LOCAL_HOSTS:
        raise HTTPException(403, "not the local dashboard")
    site = request.headers.get("sec-fetch-site")
    if site is not None and site not in ("same-origin", "none"):
        raise HTTPException(403, "cross-site request refused")


class MuteIn(BaseModel):
    muted: bool


def create_app(board: Board) -> FastAPI:
    app = FastAPI(title="Commodities board", version=__version__)
    app.state.board = board
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=sorted(LOCAL_HOSTS | {board.cfg.server.host}))
    app.mount("/ui", StaticFiles(directory=str(UI_DIR)), name="ui")

    @app.middleware("http")
    async def no_stale_pages(request: Request, call_next):
        response = await call_next(request)
        if request.url.path == "/" or request.url.path.startswith("/ui/"):
            response.headers["Cache-Control"] = "no-store, max-age=0"
        return response

    @app.get("/")
    async def index():
        page = (UI_DIR / "index.html").read_text(encoding="utf-8").replace("{{VERSION}}", __version__)
        return HTMLResponse(page, headers={"Cache-Control": "no-store, max-age=0"})

    @app.get("/api/state")
    async def state():
        return JSONResponse(board.state)

    @app.get("/api/stream")
    async def stream():
        async def gen():
            while True:
                yield f"data: {json.dumps(board.state)}\n\n"
                await asyncio.sleep(1.0)
        return StreamingResponse(gen(), media_type="text/event-stream",
                                 headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

    @app.get("/api/market/{key}")
    async def market(key: str, minutes: float = 20):
        d = board.desk(key)
        if d is None:
            raise HTTPException(404, "no such market")
        return JSONResponse(d.detail(minutes))

    @app.post("/api/market/{key}/mute", dependencies=[Depends(from_dashboard)])
    async def mute(key: str, body: MuteIn):
        try:
            board.set_muted(key, body.muted)
        except KeyError:
            raise HTTPException(404, "no such market")
        return {"key": key, "muted": body.muted}

    return app
