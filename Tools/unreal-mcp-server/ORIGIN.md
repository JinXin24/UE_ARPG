# unreal-mcp-server

Python MCP server for the `UnrealMCP` editor plugin (`Plugins/UnrealMCP`). It exposes the plugin's
commands as MCP tools and forwards them to the editor over TCP 127.0.0.1:55557.

Based on [chongdashu/unreal-mcp](https://github.com/chongdashu/unreal-mcp) (MIT), `Python/` folder at
commit 4e5f00d. Local additions: `tools/blueprint_read_tools.py`, `tools/blueprint_edit_tools.py`
(Blueprint read/edit and asset delete tools) and receive fixes in `unreal_mcp_server.py`.

Run: `uv --directory <this folder> run unreal_mcp_server.py`
