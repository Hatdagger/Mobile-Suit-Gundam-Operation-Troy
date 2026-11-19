#!/usr/bin/env python3
"""A pretend home router (UPnP Internet Gateway Device), for testing only.

Answers what the recomp's UPnP code
(thirdparty/rexglue-sdk/src/kernel/xam/live/live_upnp.cpp) asks a router:
 - SSDP M-SEARCH on a UDP port (unicast on 127.0.0.1 instead of multicast),
 - the device description XML (nested devices, relative controlURL, like
   real routers),
 - SOAP AddPortMapping / DeletePortMapping / GetExternalIPAddress, with the
   real UPnP error codes (718 conflict, 725 only permanent leases, 714 no entry).
GET /mappings lists the current mappings as JSON, for the test to check.

Usage:  python3 mock_igd.py [--port 5000] [--ssdp-port 1901]
                            [--permanent-only] [--conflict-port 7777]
Used by: optroy_netcheck upnp-selftest http://127.0.0.1:5000/rootDesc.xml 1901
"""

import argparse
import json
import re
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SERVICE = "urn:schemas-upnp-org:service:WANIPConnection:1"

DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <friendlyName>Mock Router</friendlyName>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
        <controlURL>/ctl/L3F</controlURL>
      </service>
    </serviceList>
    <deviceList>
      <device>
        <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
        <deviceList>
          <device>
            <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
            <serviceList>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
                <controlURL>ctl/CmnIfCfg</controlURL>
              </service>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
                <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
                <controlURL>ctl/IPConn</controlURL>
              </service>
            </serviceList>
          </device>
        </deviceList>
      </device>
    </deviceList>
  </device>
</root>
"""

lock = threading.Lock()
mappings = {}  # (protocol, port) -> {"client", "lease", "description"}
args = None


def arg(body, name):
    m = re.search(r"<%s>(.*?)</%s>" % (name, name), body, re.S)
    return m.group(1).strip() if m else ""


def fault(code, text):
    return (500, '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                 "<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
                 '<detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0">'
                 "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
                 "</UPnPError></detail></s:Fault></s:Body></s:Envelope>" % (code, text))


def ok(action, inner=""):
    return (200, '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                 '<s:Body><u:%sResponse xmlns:u="%s">%s</u:%sResponse></s:Body></s:Envelope>'
                 % (action, SERVICE, inner, action))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        pass

    def reply(self, status, body, ctype="text/xml"):
        data = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/rootDesc.xml":
            self.reply(200, DESCRIPTION)
        elif self.path == "/mappings":
            with lock:
                listing = [{"protocol": p, "port": port, **v} for (p, port), v in mappings.items()]
            self.reply(200, json.dumps(listing), "application/json")
        else:
            self.reply(404, "not found", "text/plain")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode(errors="replace")
        if self.path != "/ctl/IPConn":
            self.reply(404, "not found", "text/plain")
            return
        soap_action = self.headers.get("SOAPAction", "").strip('"')
        if not soap_action.startswith(SERVICE + "#"):
            self.reply(*fault(401, "Invalid Action"))
            return
        action = soap_action.split("#", 1)[1]
        if action == "AddPortMapping":
            port = int(arg(body, "NewExternalPort") or 0)
            proto = arg(body, "NewProtocol")
            lease = int(arg(body, "NewLeaseDuration") or 0)
            client = arg(body, "NewInternalClient")
            if proto not in ("UDP", "TCP") or not port or arg(body, "NewInternalPort") != str(port):
                self.reply(*fault(402, "Invalid Args"))
                return
            if args.conflict_port and port == args.conflict_port:
                self.reply(*fault(718, "ConflictInMappingEntry"))
                return
            if args.permanent_only and lease != 0:
                self.reply(*fault(725, "OnlyPermanentLeasesSupported"))
                return
            with lock:
                mappings[(proto, port)] = {"client": client, "lease": lease,
                                           "description": arg(body, "NewPortMappingDescription")}
            self.reply(*ok(action))
        elif action == "DeletePortMapping":
            key = (arg(body, "NewProtocol"), int(arg(body, "NewExternalPort") or 0))
            with lock:
                if key not in mappings:
                    self.reply(*fault(714, "NoSuchEntryInArray"))
                    return
                del mappings[key]
            self.reply(*ok(action))
        elif action == "GetExternalIPAddress":
            self.reply(*ok(action, "<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>"))
        else:
            self.reply(*fault(401, "Invalid Action"))


def ssdp_responder(port, http_port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    while True:
        data, addr = s.recvfrom(2048)
        text = data.decode(errors="replace")
        if not text.startswith("M-SEARCH"):
            continue
        st = re.search(r"^ST:\s*(.+?)\r?$", text, re.M | re.I)
        st = st.group(1).strip() if st else "upnp:rootdevice"
        if "InternetGatewayDevice:1" not in st:
            continue  # answer one of the three searches, like a real IGD v1
        reply = ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: %s\r\n"
                 "USN: uuid:mock::%s\r\nEXT:\r\nSERVER: Mock UPnP/1.1\r\n"
                 "Location: http://127.0.0.1:%d/rootDesc.xml\r\n\r\n" % (st, st, http_port))
        s.sendto(reply.encode(), addr)


def main():
    global args
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--ssdp-port", type=int, default=1901)
    parser.add_argument("--permanent-only", action="store_true")
    parser.add_argument("--conflict-port", type=int, default=0)
    args = parser.parse_args()
    threading.Thread(target=ssdp_responder, args=(args.ssdp_port, args.port), daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("mock router on http://127.0.0.1:%d/rootDesc.xml, SSDP on UDP %d"
          % (args.port, args.ssdp_port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
