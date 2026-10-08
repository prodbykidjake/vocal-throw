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
