#!/usr/bin/env python3
"""A minimal stand-in for xdg-desktop-portal: owns
org.freedesktop.portal.Desktop on the session bus and serves
org.freedesktop.portal.Settings.ReadOne from a config file.

FDK's theme layer reads the desktop font statement through this
exact interface (src/theme/ui_metrics.c, portal_readone_font_reply),
so the mock plays the role a real xdg-desktop-portal-gtk plays on a
desktop where gsettings (dconf) is the source of truth. The value
served is read fresh from PORTAL_MOCK_CONF on every call, so flipping
the file stands in for `gsettings set`.

Usage: DBUS_SESSION_BUS_ADDRESS must be set. PORTAL_MOCK_CONF
(default /tmp/fdk_portal_mock.conf) holds lines "key=value"; the
font line is font-name=Sans 11.
"""
import asyncio
import os

from dbus_next.aio.message_bus import MessageBus
from dbus_next.service import ServiceInterface, method, Variant

CONF = os.environ.get("PORTAL_MOCK_CONF", "/tmp/fdk_portal_mock.conf")


def read_conf():
    conf = {}
    try:
        with open(CONF, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    conf[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return conf


class Settings(ServiceInterface):
    def __init__(self):
        super().__init__("org.freedesktop.portal.Settings")

    @method()
    def ReadOne(self, namespace: "s", key: "s") -> "v":
        conf = read_conf()
        if key == "font-name":
            val = conf.get("font-name", "")
            print(f"[portal-mock] ReadOne {namespace}/{key} -> '{val}'",
                  flush=True)
            return Variant("s", val)
        if namespace == "org.freedesktop.appearance" and key == "color-scheme":
            return Variant("u", 0)
        return Variant("s", "")

    @method()
    def ReadAll(self, namespaces: "a{ss}") -> "a{sv}":
        return {}


async def main():
    bus = await MessageBus().connect()
    bus.export("/org/freedesktop/portal/desktop", Settings())
    await bus.request_name("org.freedesktop.portal.Desktop")
    print("[portal-mock] serving on the session bus", flush=True)
    await asyncio.get_event_loop().create_future()  # serve forever


if __name__ == "__main__":
    asyncio.run(main())
