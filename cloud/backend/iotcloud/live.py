"""Fan-out of live events (a message was stored) to every open browser stream (server-sent events)."""
import asyncio
import json


class Hub:
    def __init__(self):
        self._loop = None
        self._queues: set[asyncio.Queue] = set()

    def bind(self, loop):
        self._loop = loop

    def publish(self, event: dict):
        """Called from the MQTT thread."""
        if self._loop is None:
            return
        for q in list(self._queues):
            self._loop.call_soon_threadsafe(self._offer, q, event)

    @staticmethod
    def _offer(q, event):
        if q.full():  # a slow browser loses the oldest event instead of growing memory
            q.get_nowait()
        q.put_nowait(event)

    async def stream(self):
        q: asyncio.Queue = asyncio.Queue(maxsize=200)
        self._queues.add(q)
        try:
            yield "retry: 3000\n\n"
            while True:
                try:
                    event = await asyncio.wait_for(q.get(), timeout=15)
                    yield f"data: {json.dumps(event)}\n\n"
                except asyncio.TimeoutError:
                    yield ": keepalive\n\n"
        finally:
            self._queues.discard(q)
