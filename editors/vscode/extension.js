// The Seed extension's language client: it starts `seed lsp` (the language
// server built into the seed binary, compiler/README.md, "The language
// server") for .sd files, through vscode-languageclient. The grammar and
// the language settings work without it.
"use strict";

const vscode = require("vscode");

let client = null;

function activate(context) {
  const config = vscode.workspace.getConfiguration("seed");
  if (!config.get("languageServer.enabled", true)) {
    return;
  }
  let languageclient;
  try {
    languageclient = require("vscode-languageclient/node");
  } catch (err) {
    vscode.window.showWarningMessage(
      "Seed: the language server client is not installed; run `npm install` in editors/vscode (syntax highlighting works without it)."
    );
    return;
  }
  const command = config.get("path", "seed") || "seed";
  const executable = { command: command, args: ["lsp"], transport: languageclient.TransportKind.stdio };
  const serverOptions = { run: executable, debug: executable };
  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "seed" }],
  };
  client = new languageclient.LanguageClient("seed", "Seed language server", serverOptions, clientOptions);
  client.start().catch((err) => {
    vscode.window.showErrorMessage(
      `Seed: could not start "${command} lsp": ${err && err.message ? err.message : err}. Set seed.path to the seed binary (build/seed2 in the repository, or seed after make install).`
    );
  });
  context.subscriptions.push({
    dispose: () => {
      if (client) {
        client.stop();
      }
    },
  });
}

function deactivate() {
  if (!client) {
    return undefined;
  }
  const stopping = client.stop();
  client = null;
  return stopping;
}

module.exports = { activate, deactivate };
