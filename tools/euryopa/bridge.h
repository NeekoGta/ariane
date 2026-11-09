#ifndef BRIDGE_H
#define BRIDGE_H

// Local control bridge for AI assistants (tools/mcp/ariane_mcp.py).
//
// Ariane listens on 127.0.0.1 only (port 47811, or ARIANE_BRIDGE_PORT). A
// client sends one command per line, "name<TAB>key=value<TAB>key=value...", and
// gets one JSON line back. The commands run on the main thread, between two
// frames, through the editor's own functions (undo, save, LOD links).

namespace Bridge {

const int DEFAULT_PORT = 47811;

void SetEnabled(bool enabled);	// starts or stops the server (once ready)
void Ready(void);		// the map is loaded: start if enabled
bool IsEnabled(void);
bool IsRunning(void);
int Port(void);
const char *LastError(void);
int NumCommands(void);

void Process(void);	// in Draw(), before the render list is built
void Capture(void);	// in Draw(), once the 3D view is drawn, before the UI
void Shutdown(void);

}

// gui.cpp: Ctrl+S, with its toast.
bool SaveMapFromBridge(void);

#endif
