"""FastAPI server: the board's landing page, one SSE stream for every card, and a detail endpoint per market."""
from __future__ import annotations

import asyncio
import json
import pathlib

from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.middleware.trustedhost import TrustedHostMiddleware
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, RedirectResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import __version__
from .board import Board
from .phone import COOKIE, key_matches

UI_DIR = pathlib.Path(__file__).resolve().parents[1] / "ui"
OWN_HEADER = "x-cx15m"
LOCAL_HOSTS = {"127.0.0.1", "localhost", "::1", "[::1]"}


def _host_name(request: Request) -> str:
    raw = request.headers.get("host") or ""
    return raw.split("]")[0] + "]" if raw.startswith("[") else raw.rsplit(":", 1)[0]


async def from_dashboard(request: Request):
    """State-changing calls must carry the board's own header and come from the board's own local origin (a page in
    another tab cannot add the header cross-site without a CORS preflight, which this server never answers). A phone
    that opened the private phone link counts as the board too."""
    if request.headers.get(OWN_HEADER) != "1":
        raise HTTPException(403, "missing dashboard header")
    if _host_name(request) not in LOCAL_HOSTS and not getattr(request.state, "phone_ok", False):
        raise HTTPException(403, "not the local dashboard")
    site = request.headers.get("sec-fetch-site")
    if site is not None and site not in ("same-origin", "none"):
        raise HTTPException(403, "cross-site request refused")


class MuteIn(BaseModel):
    muted: bool


class ModeIn(BaseModel):
    mode: str


def create_app(board: Board, phone_key: str | None = None) -> FastAPI:
    app = FastAPI(title="Crypto board", version=__version__)
    app.state.board = board
    if phone_key is None:
        app.add_middleware(TrustedHostMiddleware, allowed_hosts=sorted(LOCAL_HOSTS | {board.cfg.server.host}))
    app.mount("/ui", StaticFiles(directory=str(UI_DIR)), name="ui")

    @app.middleware("http")
    async def no_stale_pages(request: Request, call_next):
        response = await call_next(request)
        if request.url.path == "/" or request.url.path.startswith("/ui/"):
            response.headers["Cache-Control"] = "no-store, max-age=0"
        return response

    if phone_key is not None:
        @app.middleware("http")
        async def phone_gate(request: Request, call_next):
            """Phone mode: the Mac itself gets in as always; any other device needs the phone link's key. Opening
            the link stores the key in a cookie (a year) and drops it from the address bar. A DNS-rebound page has
            its own name in Host, so the browser never sends it this cookie."""
            request.state.phone_ok = False
            client = request.client.host if request.client else ""
            if _host_name(request) in LOCAL_HOSTS and (client.startswith("127.") or client == "::1"):
                return await call_next(request)  # the Mac itself (a device on the Wi-Fi can fake Host, not its address)
            if key_matches(request.query_params.get("key"), phone_key):
                response = RedirectResponse(request.url.path or "/", status_code=303)
                response.set_cookie(COOKIE, phone_key, max_age=365 * 86400, httponly=True, samesite="lax")
                return response
            if key_matches(request.cookies.get(COOKIE), phone_key):
                request.state.phone_ok = True
                return await call_next(request)
            return PlainTextResponse("Locked. Open the phone link shown in the board's terminal on your Mac.", 403)

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

    @app.post("/api/mode", dependencies=[Depends(from_dashboard)])
    async def set_mode(body: ModeIn):
        try:
            board.set_mode(body.mode)
        except ValueError as e:
            raise HTTPException(400, str(e))
        return {"mode": board.mode}

    @app.post("/api/market/{key}/mute", dependencies=[Depends(from_dashboard)])
    async def mute(key: str, body: MuteIn):
        try:
            board.set_muted(key, body.muted)
        except KeyError:
            raise HTTPException(404, "no such market")
        return {"key": key, "muted": body.muted}

    return app
