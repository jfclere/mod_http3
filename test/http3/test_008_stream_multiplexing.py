"""Test HTTP/3 stream multiplexing over a single connection."""

import asyncio
import re
import ssl

import pytest

from .env import H3Conf

from aioquic.asyncio.client import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration

N_CONCURRENT_GETS = 20
POST_BODY_LEN = 100_000


class _MuxClient(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = H3Connection(self._quic)
        self.status = {}
        self.body = {}
        self.done = {}

    def quic_event_received(self, event):
        for h3_event in self._http.handle_event(event):
            sid = h3_event.stream_id
            if isinstance(h3_event, HeadersReceived):
                for k, v in h3_event.headers:
                    if k == b":status":
                        self.status[sid] = v.decode()
                if h3_event.stream_ended:
                    self.done[sid].set()
            elif isinstance(h3_event, DataReceived):
                self.body[sid] = self.body.get(sid, b"") + h3_event.data
                if h3_event.stream_ended:
                    self.done[sid].set()

    def start_get(self, authority: str, path: str) -> int:
        sid = self._quic.get_next_available_stream_id(is_unidirectional=False)
        self.done[sid] = asyncio.Event()
        self._http.send_headers(
            stream_id=sid,
            headers=[
                (b":method", b"GET"),
                (b":scheme", b"https"),
                (b":authority", authority.encode()),
                (b":path", path.encode()),
            ],
            end_stream=True,
        )
        return sid

    def start_post(self, authority: str, path: str, body: bytes) -> int:
        sid = self._quic.get_next_available_stream_id(is_unidirectional=False)
        self.done[sid] = asyncio.Event()
        self._http.send_headers(
            stream_id=sid,
            headers=[
                (b":method", b"POST"),
                (b":scheme", b"https"),
                (b":authority", authority.encode()),
                (b":path", path.encode()),
                (b"content-length", str(len(body)).encode()),
            ],
            end_stream=False,
        )
        # Send body in chunks to interleave data frames.
        chunk_size = 4096
        for i in range(0, len(body), chunk_size):
            end = (i + chunk_size) >= len(body)
            self._http.send_data(sid, body[i : i + chunk_size], end)
        return sid


class TestStreamMultiplexing:
    @pytest.fixture(autouse=True, scope="class")
    def _class_scope(self, env):
        H3Conf(env).add_vhost_test1().install()
        assert env.apache_restart() == 0

    def test_001_many_concurrent_streams_one_connection(self, env):
        authority = f"test1.{env.http_tld}"

        async def run():
            config = QuicConfiguration(
                is_client=True, alpn_protocols=H3_ALPN, verify_mode=ssl.CERT_NONE, server_name=authority
            )
            async with connect(
                env.http_addr, env.https_port, configuration=config, create_protocol=_MuxClient
            ) as client:
                get_ids = [client.start_get(authority, "/index.html") for _ in range(N_CONCURRENT_GETS)]
                post_body = bytes((i % 251) for i in range(POST_BODY_LEN))
                post_id = client.start_post(authority, "/upload-does-not-exist", post_body)
                client.transmit()

                await asyncio.wait_for(
                    asyncio.gather(*(client.done[sid].wait() for sid in [*get_ids, post_id])), timeout=15
                )
                return client, get_ids, post_id

        client, get_ids, post_id = asyncio.run(run())

        for sid in get_ids:
            assert client.status[sid] == "200", f"stream {sid}: status={client.status.get(sid)}"
            assert b"mod_http3 test" in client.body[sid]

        # POST response can be 404/405, but connection must remain healthy.
        assert client.status[post_id] in ("200", "404", "405")

        pattern = re.compile(rf".*\[http3:info].*POST body={POST_BODY_LEN}.*")
        env.httpd_error_log.scan_recent(pattern, timeout=5)
