"""requests transport backed by the guest's TCP stack and BearSSL trust store."""
import http.client
import io
from urllib.parse import urlsplit

import _lixin_http
from pip._vendor.requests import Response
from pip._vendor.requests.adapters import BaseAdapter
from pip._vendor.requests.exceptions import ConnectionError
from pip._vendor.requests.structures import CaseInsensitiveDict
from pip._vendor.urllib3._collections import HTTPHeaderDict
from pip._vendor.urllib3.response import HTTPResponse


class SocketReader(io.RawIOBase):
    def __init__(self, connection):
        self.connection = connection

    def readable(self):
        return True

    def readinto(self, buffer):
        data = _lixin_http.receive(self.connection, min(len(buffer), 4096))
        buffer[:len(data)] = data
        return len(data)

    def makefile(self, mode):
        return io.BufferedReader(self)

    def close(self):
        self.connection = None
        super().close()


class GuestAdapter(BaseAdapter):
    def send(self, request, **options):
        url = urlsplit(request.url)
        secure = url.scheme == "https"
        if request.method not in ("GET", "HEAD") or options.get("proxies") or options.get("cert"):
            raise ConnectionError("LixinOS pip supports direct HTTP(S) GET and HEAD requests")
        if isinstance(options.get("verify"), str):
            raise ConnectionError("LixinOS pip uses the system TLS trust store")
        headers = CaseInsensitiveDict(request.headers)
        headers.update({"Host": url.netloc.rsplit("@", 1)[-1], "Connection": "close",
                        "Accept-Encoding": "identity"})
        wire = [f"{request.method} {request.path_url} HTTP/1.1"]
        for key, value in headers.items():
            if not http.client._is_legal_header_name(key.encode("ascii")) or any(c in value for c in "\r\n"):
                raise ConnectionError("invalid HTTP header")
            wire.append(f"{key}: {value}")
        try:
            data = ("\r\n".join(wire) + "\r\n\r\n").encode("latin-1")
            connection = _lixin_http.request(url.hostname, url.port or (443 if secure else 80), secure, data)
            message = http.client.HTTPResponse(SocketReader(connection), method=request.method)
            message.begin()
        except (OSError, http.client.HTTPException) as exc:
            raise ConnectionError(str(exc), request=request) from exc
        response = Response()
        response.status_code = message.status
        response.reason = message.reason
        response.headers = CaseInsensitiveDict(message.getheaders())
        response.raw = HTTPResponse(body=message, original_response=message, headers=HTTPHeaderDict(message.getheaders()),
                                    status=message.status, preload_content=False, decode_content=False)
        response.url = request.url
        response.request = request
        response.connection = self
        return response

    def close(self):
        pass
