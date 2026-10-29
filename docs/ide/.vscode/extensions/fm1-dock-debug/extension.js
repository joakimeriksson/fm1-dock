const vscode = require("vscode");
const path = require("path");

class FM1DebugAdapterFactory {
    createDebugAdapterDescriptor(session) {
        const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || "";

        const script = path.join(workspaceRoot, "tools", "fm1dap.py");
        const args = [script];
        if (session.configuration.emu) {
            args.push("--emu");
        } else if (session.configuration.mock) {
            args.push("--mock");
        } else if (session.configuration.ram) {
            args.push("--ram", path.resolve(workspaceRoot, session.configuration.ram));
        } else if (session.configuration.port) {
            args.push("--port", session.configuration.port);
        }
        return new vscode.DebugAdapterExecutable("python3", args);
    }
}

function activate(context) {
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory(
            "fm1-dock",
            new FM1DebugAdapterFactory()
        )
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
