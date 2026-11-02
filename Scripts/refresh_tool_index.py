"""Generate a compact cross-server MCP index; no asset operations or tool execution."""
import argparse
import json
import re
from pathlib import Path
from datetime import datetime, timezone
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def short(text):
    text = re.split(r"\n\s*\n|\n\s*Args:", text.strip())[0]
    text = re.sub(r"\s+", " ", text).replace("|", "\\|")
    return text if len(text) <= 150 else text[:147].rsplit(" ", 1)[0] + "..."


def capture_official(url):
    session = None
    counter = 0

    def rpc(method, params):
        nonlocal session, counter
        counter += 1
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if session:
            headers["Mcp-Session-Id"] = session
        request = urllib.request.Request(url, json.dumps({"jsonrpc": "2.0", "id": counter,
            "method": method, "params": params}).encode(), headers)
        with urllib.request.urlopen(request, timeout=60) as response:
            body = response.read().decode()
            session = response.headers.get("Mcp-Session-Id", session)
        if body.startswith(("event:", "data:")):
            body = next(line[5:].strip() for line in body.splitlines() if line.startswith("data:"))
        result = json.loads(body)
        if "error" in result:
            raise RuntimeError(result["error"])
        return result["result"]

    def call(name, arguments):
        result = rpc("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(result)
        return "\n".join(c["text"] for c in result.get("content", []) if c["type"] == "text")

    rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
        "clientInfo": {"name": "mcp-tool-index", "version": "1"}})
    listing = call("list_toolsets", {})
    # Toolset descriptions contain their own bullets; registered toolset IDs contain dots.
    names = re.findall(r"^- ([\w.]+\.[\w.]+):", listing, re.M)
    if not names:
        raise RuntimeError("No official toolsets discovered; existing snapshot was preserved")
    toolsets = []
    for name in sorted(set(names)):
        definition = json.loads(call("describe_toolset", {"toolset_name": name}))
        toolsets.append({"name": name, "tools": [{"name": t["name"],
            "description": t.get("description", "")} for t in definition["tools"]]})
    return {"capturedUtc": datetime.now(timezone.utc).isoformat(),
        "source": "official MCP list_toolsets + describe_toolset", "toolsets": toolsets}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--official-url", help="Official MCP HTTP endpoint; refreshes metadata using read-only discovery")
    parser.add_argument("--official-snapshot", type=Path, default=ROOT / "docs/official-tools.snapshot.json")
    parser.add_argument("--custom-catalog", type=Path, default=ROOT / "Adapter/UnrealMCP.Adapter/unreal-tools.json")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/MCP-Tool-Index.md")
    args = parser.parse_args()
    official = capture_official(args.official_url) if args.official_url else json.loads(args.official_snapshot.read_text(encoding="utf-8-sig"))
    custom = json.loads(args.custom_catalog.read_text(encoding="utf-8-sig"))
    rows = [("Custom", "native registry", t["name"], t.get("description", "")) for t in custom["tools"]]
    for ts in official["toolsets"]:
        for tool in ts["tools"]:
            prefix = ts["name"] + "."
            name = tool["name"][len(prefix):] if tool["name"].startswith(prefix) else tool["name"]
            rows.append(("Official", ts["name"], name, tool.get("description", "")))
    if len({r[:3] for r in rows}) != len(rows):
        raise RuntimeError("Duplicate tool routes; existing index was preserved")
    lines = ["# MCP Tool Index", "", "Generated metadata only; fetch the exact live schema before execution.", "",
        f"Custom catalog captured: {custom.get('capturedUtc', 'unknown')}. Official catalog captured: {official['capturedUtc']}.",
        f"Contains {len(custom['tools'])} custom tools and {len(rows) - len(custom['tools'])} official tools across {len(official['toolsets'])} toolsets.", "",
        "Search this file with `rg -i 'interface|delete.*node|merge'`. Descriptions are shortened; a missing keyword is not proof a capability is absent.", "",
        "Custom route: `unreal.adapter.SearchTools` -> `GetToolSchema` -> `CallTool` (name is the native tool name). Full adapter mode also exposes native tools directly.",
        "Official route: `describe_toolset(toolset_name)` for the exact schema, then `call_tool(toolset_name, tool_name, arguments)`.",
        "If missing: refresh the custom manifest and re-list, or list official toolsets and describe the relevant one. Cached metadata does not prove current availability.", "",
        "Refresh from snapshots: `python Scripts/refresh_tool_index.py`. Refresh official metadata live: add `--official-url <MCP HTTP endpoint>`. To use a freshly captured custom catalog, add `--custom-catalog <path>`.", "",
        "| Server | Toolset | Tool | Short description |", "|---|---|---|---|"]
    lines += [f"| {server} | `{toolset}` | `{name}` | {short(desc)} |" for server, toolset, name, desc in sorted(rows)]
    if args.official_url:
        args.official_snapshot.write_text(json.dumps(official, indent=2) + "\n", encoding="utf-8")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {len(rows)} tool routes to {args.output}")


if __name__ == "__main__":
    main()
