"""
Blueprint Edit Tools for Unreal MCP.

Tools for changing Blueprint graphs, variables, functions and components. Nodes and pins are
addressed by the node names that read_blueprint returns (e.g. "K2Node_CallFunction_3") and pin
references look like "NodeName.PinName". Every edit is one undoable editor transaction (Ctrl+Z),
and by default the Blueprint is recompiled afterwards with errors/warnings returned under "compile".
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")


def _send(command: str, params: Dict[str, Any]) -> Dict[str, Any]:
    """Send a command to the editor plugin, dropping parameters that were not given."""
    from unreal_mcp_server import get_unreal_connection

    try:
        unreal = get_unreal_connection()
        if not unreal:
            logger.error("Failed to connect to Unreal Engine")
            return {"success": False, "message": "Failed to connect to Unreal Engine"}

        response = unreal.send_command(command, {k: v for k, v in params.items() if v is not None})
        if not response:
            logger.error("No response from Unreal Engine")
            return {"success": False, "message": "No response from Unreal Engine"}
        return response

    except Exception as e:
        error_msg = f"Error running {command}: {e}"
        logger.error(error_msg)
        return {"success": False, "message": error_msg}


def register_blueprint_edit_tools(mcp: FastMCP):
    """Register Blueprint edit tools with the MCP server."""

    @mcp.tool()
    def add_blueprint_nodes(
        ctx: Context,
        blueprint: str,
        nodes: List[Dict[str, Any]],
        graph: str = "EventGraph",
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Add nodes to a Blueprint graph. Returns the created nodes with their names and pins.

        Each entry in nodes has a "type", optional "x"/"y", and type-specific fields:
          call_function  function, owner (optional class, e.g. "Actor", "KismetMathLibrary"; K2_ prefix is tried too)
          variable_get / variable_set  variable, owner (optional, for another class's variable)
          event          event (e.g. "BeginPlay", "Tick", "ActorBeginOverlap"); returns the existing node if already overridden
          custom_event   name
          branch, sequence, self, reroute
          cast           class (e.g. "BP_Enemy", "Character")
          macro          macro from StandardMacros (e.g. "ForEachLoop", "ForLoop", "DoOnce", "IsValid", "Gate")
          make_struct / break_struct  struct (e.g. "FVector", "FHitResult")
          node           class (any K2Node class, e.g. "K2Node_SpawnActorFromClass") plus optional "properties"
        Any entry may carry "properties": {"PropertyName": "value in UE text format"}.

        Args:
            blueprint: Blueprint name or path
            nodes: Node specs as described above
            graph: Graph name (event graph or function name)
            compile: Recompile afterwards and report errors
        """
        return _send("add_blueprint_nodes", {"blueprint": blueprint, "graph": graph, "nodes": nodes, "compile": compile})

    @mcp.tool()
    def paste_blueprint_nodes(
        ctx: Context,
        blueprint: str,
        t3d: str,
        graph: str = "EventGraph",
        x: Optional[float] = None,
        y: Optional[float] = None,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Paste nodes from editor copy/paste text (T3D, what Ctrl+C produces), exactly like Ctrl+V.

        Pins and LinkedTo may be omitted: pins are rebuilt on paste, then use connect_blueprint_pins.
        read_blueprint(format="t3d") returns text in this format.

        Args:
            blueprint: Blueprint name or path
            t3d: The "Begin Object ... End Object" text
            graph: Target graph
            x, y: Optional position for the top-left corner of the pasted block
            compile: Recompile afterwards and report errors
        """
        return _send("paste_blueprint_nodes", {"blueprint": blueprint, "graph": graph, "t3d": t3d, "x": x, "y": y, "compile": compile})

    @mcp.tool()
    def delete_blueprint_nodes(
        ctx: Context,
        blueprint: str,
        nodes: List[str],
        graph: str = "EventGraph",
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Delete nodes (by node name) from a Blueprint graph.

        Args:
            blueprint: Blueprint name or path
            nodes: Node names, e.g. ["K2Node_CallFunction_3"]
            graph: Graph containing the nodes
            compile: Recompile afterwards and report errors
        """
        return _send("delete_blueprint_nodes", {"blueprint": blueprint, "graph": graph, "nodes": nodes, "compile": compile})

    @mcp.tool()
    def connect_blueprint_pins(
        ctx: Context,
        blueprint: str,
        connections: List[Dict[str, str]],
        graph: str = "EventGraph",
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Wire pins together. Uses the editor's own rules, so conversion nodes are added when needed
        and exec outputs replace their previous wire.

        Args:
            blueprint: Blueprint name or path
            connections: [{"from": "NodeA.then", "to": "NodeB.execute"}, ...] (output -> input)
            graph: Graph containing the nodes
            compile: Recompile afterwards and report errors
        """
        return _send("connect_blueprint_pins", {"blueprint": blueprint, "graph": graph, "connections": connections, "compile": compile})

    @mcp.tool()
    def disconnect_blueprint_pins(
        ctx: Context,
        blueprint: str,
        graph: str = "EventGraph",
        connections: Optional[List[Dict[str, str]]] = None,
        pins: Optional[List[str]] = None,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Remove wires: specific ones via connections [{"from", "to"}], or every wire on the pins listed in pins.

        Args:
            blueprint: Blueprint name or path
            graph: Graph containing the nodes
            connections: Wires to remove
            pins: "NodeName.PinName" entries whose wires are all removed
            compile: Recompile afterwards and report errors
        """
        return _send("disconnect_blueprint_pins", {"blueprint": blueprint, "graph": graph, "connections": connections, "pins": pins, "compile": compile})

    @mcp.tool()
    def set_blueprint_pin_defaults(
        ctx: Context,
        blueprint: str,
        values: List[Dict[str, str]],
        graph: str = "EventGraph",
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Set default values of unconnected input pins.

        Values use the editor's text format: "1.5", "true", "Hello", "(X=1,Y=2,Z=3)" for vectors,
        enum entry names, or an asset/class path for object pins ("" clears it).

        Args:
            blueprint: Blueprint name or path
            values: [{"pin": "NodeName.PinName", "value": "..."}, ...]
            graph: Graph containing the nodes
            compile: Recompile afterwards and report errors
        """
        return _send("set_blueprint_pin_defaults", {"blueprint": blueprint, "graph": graph, "values": values, "compile": compile})

    @mcp.tool()
    def move_blueprint_nodes(
        ctx: Context,
        blueprint: str,
        moves: List[Dict[str, Any]],
        graph: str = "EventGraph"
    ) -> Dict[str, Any]:
        """
        Move nodes to new graph positions.

        Args:
            blueprint: Blueprint name or path
            moves: [{"node": "NodeName", "x": 400, "y": 200}, ...]
            graph: Graph containing the nodes
        """
        return _send("move_blueprint_nodes", {"blueprint": blueprint, "graph": graph, "moves": moves})

    @mcp.tool()
    def add_blueprint_comment(
        ctx: Context,
        blueprint: str,
        text: str,
        graph: str = "EventGraph",
        nodes: Optional[List[str]] = None,
        x: Optional[float] = None,
        y: Optional[float] = None,
        width: Optional[float] = None,
        height: Optional[float] = None,
        color: Optional[List[float]] = None
    ) -> Dict[str, Any]:
        """
        Add a comment box, either wrapped around the given nodes or at an explicit rectangle.

        Args:
            blueprint: Blueprint name or path
            text: Comment text
            graph: Target graph
            nodes: Node names to enclose (box is sized around them)
            x, y, width, height: Rectangle when nodes is not given
            color: [r, g, b, a] in 0..1
        """
        return _send("add_blueprint_comment", {
            "blueprint": blueprint, "graph": graph, "text": text, "nodes": nodes,
            "x": x, "y": y, "width": width, "height": height, "color": color
        })

    @mcp.tool()
    def auto_layout_blueprint_graph(
        ctx: Context,
        blueprint: str,
        graph: str = "EventGraph",
        nodes: Optional[List[str]] = None
    ) -> Dict[str, Any]:
        """
        Tidy node positions: exec flow runs left to right in columns, data nodes sit left of the node
        that uses them, and each event chain gets its own horizontal band. Comment boxes are not moved.

        Args:
            blueprint: Blueprint name or path
            graph: Graph to arrange
            nodes: Only arrange these nodes (default: all nodes except comments)
        """
        return _send("auto_layout_blueprint_graph", {"blueprint": blueprint, "graph": graph, "nodes": nodes})

    @mcp.tool()
    def add_blueprint_function(
        ctx: Context,
        blueprint: str,
        name: str,
        inputs: Optional[List[Dict[str, str]]] = None,
        outputs: Optional[List[Dict[str, str]]] = None,
        pure: bool = False,
        const: bool = False,
        access: str = "public",
        category: Optional[str] = None,
        tooltip: Optional[str] = None,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Create a function graph with parameters. Fill its body with add_blueprint_nodes using graph=name;
        the returned entry/result node names are where the exec flow starts and ends.

        Types use C++ spelling: bool, int32, int64, float, double, FName, FString, FText, FVector,
        FRotator, FTransform, AActor*, UObject*, TSubclassOf<AActor>, TSoftObjectPtr<UTexture2D>,
        TArray<...>, TSet<...>, TMap<K, V>, enum names, Blueprint classes (BP_Enemy*).

        Args:
            blueprint: Blueprint name or path
            name: Function name
            inputs: [{"name": "Damage", "type": "float"}, ...]
            outputs: [{"name": "bKilled", "type": "bool"}, ...]
            pure: Blueprint pure function (no exec pins)
            const: Const function
            access: public, protected or private
            category: Category shown in the palette
            tooltip: Function description
            compile: Recompile afterwards and report errors
        """
        return _send("add_blueprint_function", {
            "blueprint": blueprint, "name": name, "inputs": inputs, "outputs": outputs, "pure": pure,
            "const": const, "access": access, "category": category, "tooltip": tooltip, "compile": compile
        })

    @mcp.tool()
    def remove_blueprint_graph(
        ctx: Context,
        blueprint: str,
        graph: str,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Delete a function, macro or extra event graph from a Blueprint.

        Args:
            blueprint: Blueprint name or path
            graph: Graph (function) name
            compile: Recompile afterwards and report errors
        """
        return _send("remove_blueprint_graph", {"blueprint": blueprint, "graph": graph, "compile": compile})

    @mcp.tool()
    def rename_blueprint_graph(
        ctx: Context,
        blueprint: str,
        graph: str,
        new_name: str,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Rename a function or graph; call sites are updated by the editor.

        Args:
            blueprint: Blueprint name or path
            graph: Current graph name
            new_name: New name
            compile: Recompile afterwards and report errors
        """
        return _send("rename_blueprint_graph", {"blueprint": blueprint, "graph": graph, "new_name": new_name, "compile": compile})

    @mcp.tool()
    def add_blueprint_variables(
        ctx: Context,
        blueprint: str,
        variables: List[Dict[str, Any]],
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Add member variables.

        Each entry: {"name", "type" (C++ spelling, see add_blueprint_function), optional "default"
        (UE text format, e.g. "100.0", "True", "(X=0,Y=0,Z=1)"), "category", "instance_editable",
        "tooltip", "expose_on_spawn"}.

        Args:
            blueprint: Blueprint name or path
            variables: Variable specs
            compile: Recompile afterwards (needed for defaults to be applied)
        """
        return _send("add_blueprint_variables", {"blueprint": blueprint, "variables": variables, "compile": compile})

    @mcp.tool()
    def remove_blueprint_variable(
        ctx: Context,
        blueprint: str,
        name: str,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Remove a member variable (get/set nodes that used it will show up as compile errors).

        Args:
            blueprint: Blueprint name or path
            name: Variable name
            compile: Recompile afterwards and report errors
        """
        return _send("remove_blueprint_variable", {"blueprint": blueprint, "name": name, "compile": compile})

    @mcp.tool()
    def modify_blueprint_variable(
        ctx: Context,
        blueprint: str,
        name: str,
        new_name: Optional[str] = None,
        type: Optional[str] = None,
        default: Optional[str] = None,
        category: Optional[str] = None,
        instance_editable: Optional[bool] = None,
        tooltip: Optional[str] = None,
        expose_on_spawn: Optional[bool] = None,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Change a member variable: rename (references are updated), change type, default value or settings.
        Only the given fields change.

        Args:
            blueprint: Blueprint name or path
            name: Current variable name
            new_name: New name
            type: New type in C++ spelling
            default: New default value in UE text format
            category: Category
            instance_editable: Editable on placed instances
            tooltip: Tooltip
            expose_on_spawn: Show as a pin on Spawn Actor nodes
            compile: Recompile afterwards and report errors
        """
        return _send("modify_blueprint_variable", {
            "blueprint": blueprint, "name": name, "new_name": new_name, "type": type, "default": default,
            "category": category, "instance_editable": instance_editable, "tooltip": tooltip,
            "expose_on_spawn": expose_on_spawn, "compile": compile
        })

    @mcp.tool()
    def remove_blueprint_component(
        ctx: Context,
        blueprint: str,
        name: str,
        compile: bool = True
    ) -> Dict[str, Any]:
        """
        Remove a component added in this Blueprint; its children move up to its parent.

        Args:
            blueprint: Blueprint name or path
            name: Component name
            compile: Recompile afterwards and report errors
        """
        return _send("remove_blueprint_component", {"blueprint": blueprint, "name": name, "compile": compile})

    @mcp.tool()
    def compile_blueprint_report(
        ctx: Context,
        blueprint: str
    ) -> Dict[str, Any]:
        """
        Compile a Blueprint and return its status with error and warning messages.

        Args:
            blueprint: Blueprint name or path
        """
        return _send("compile_blueprint_report", {"blueprint": blueprint})

    @mcp.tool()
    def save_blueprint(
        ctx: Context,
        blueprint: str
    ) -> Dict[str, Any]:
        """
        Save a Blueprint asset to disk (edits are otherwise only in memory until the user saves).

        Args:
            blueprint: Blueprint name or path
        """
        return _send("save_blueprint", {"blueprint": blueprint})

    @mcp.tool()
    def delete_asset(
        ctx: Context,
        asset: str,
        force: bool = False,
        dry_run: bool = False
    ) -> Dict[str, Any]:
        """
        Delete a project asset (Blueprint, material, texture, level...) under /Game.

        Refuses when other assets still reference it, listing them; force=True deletes anyway.
        Deleting cannot be undone with Ctrl+Z (restore from git instead). Use dry_run=True to
        only list the referencers.

        Args:
            asset: Asset name (must be unique) or path, e.g. "/Game/Blueprints/BP_Test"
            force: Delete even if other assets reference it
            dry_run: Only report what would be deleted and who references it
        """
        return _send("delete_asset", {"asset": asset, "force": force, "dry_run": dry_run})

    logger.info("Blueprint edit tools registered successfully")
