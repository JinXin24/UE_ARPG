"""
Blueprint Read Tools for Unreal MCP.

This module provides read-only tools for inspecting Blueprint assets: listing them and
dumping their variables, components, class defaults and graph logic (nodes, pins, links).
"""

import logging
from typing import Dict, Any
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_blueprint_read_tools(mcp: FastMCP):
    """Register Blueprint read tools with the MCP server."""

    @mcp.tool()
    def list_blueprints(
        ctx: Context,
        path: str = "/Game",
        name_filter: str = ""
    ) -> Dict[str, Any]:
        """
        List Blueprint assets (Actor, Widget, Anim, Interface, Function Library...) in the project.

        Args:
            path: Content folder to search recursively (e.g. "/Game/Characters")
            name_filter: Optional substring the asset name must contain

        Returns:
            Blueprints with name, full asset path, asset class and parent class (max 500)
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("list_blueprints", {
                "path": path,
                "name_filter": name_filter
            })

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error listing blueprints: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def read_blueprint(
        ctx: Context,
        blueprint: str,
        graph: str = "",
        include_graphs: bool = True,
        format: str = "json"
    ) -> Dict[str, Any]:
        """
        Read a Blueprint's internals so its logic can be understood or converted to C++.

        Returns parent class, interfaces, variables (type, default, flags), components
        (hierarchy and edited properties), overrides of inherited components, edited class
        defaults, timelines (tracks and keys), the widget tree for Widget Blueprints, and graphs.
        Each graph lists nodes with id, class, title, function/variable/event info and pins.
        Pin links are "NodeId.PinName"; unlinked inputs show their default value.

        Args:
            blueprint: Asset name (e.g. "BP_Hero") or full path (e.g. "/Game/Characters/BP_Hero")
            graph: Only return this graph (e.g. "EventGraph" or a function name); other sections are skipped
            include_graphs: False to list graph names and node counts only (use first on big Blueprints)
            format: "json" (structured nodes), "t3d" (raw editor copy/paste text) or "both"

        Returns:
            Dict describing the Blueprint
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            response = unreal.send_command("read_blueprint", {
                "blueprint": blueprint,
                "graph": graph,
                "include_graphs": include_graphs,
                "format": format
            })

            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}

            return response

        except Exception as e:
            error_msg = f"Error reading blueprint: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    logger.info("Blueprint read tools registered successfully")
