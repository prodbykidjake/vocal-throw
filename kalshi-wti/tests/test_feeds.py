import time

from wti15m.feeds.base import TickBuffer


def test_tick_buffer_rejects_future_timestamps_and_keeps_live_ticks():
    buf = TickBuffer()
    now = time.time()
    buf.add(now - 60, 88.6)
    buf.add(now + 40, 88.668)  # an in-progress candle's end time: must be ignored
    buf.add(now, 88.75)
    assert buf.latest().price == 88.75
    assert buf.price_near(now - 60, 5) == 88.6
    assert buf.price_near(now - 30, 5) is None  # no tick within 5 s before that time
    assert buf.last_at_or_before(now - 30).price == 88.6


def test_publish_skips_subscribers_for_rejected_ticks():
    from wti15m.feeds.base import PriceFeed

    class F(PriceFeed):
        async def start(self): ...
        async def stop(self): ...

    f = F()
    got = []
    f.subscribe(lambda t: got.append(t.price))
    now = time.time()
    assert f.buffer.add(now, 90.0) is True
    f._publish(now - 30, 85.0)  # older than what we have: must not reach the volatility estimator
    f._publish(now + 1, 90.1)
    assert got == [90.1] and f.active_name == "feed"
