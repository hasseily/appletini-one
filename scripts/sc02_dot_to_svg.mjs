#!/usr/bin/env node
/** Render every SC-02 Graphviz DOT file to a deterministic SVG file. */

import { mkdir, readFile, rename, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { pathToFileURL } from "node:url";


const DEFAULT_VIZ_MODULE = new URL(
    "./sc02_dot_renderer/node_modules/@viz-js/viz/dist/viz.js",
    import.meta.url
).href;
const EXPECTED_GRAPHVIZ_VERSION = "14.1.3";

function usage() {
    console.error(
        "usage: node sc02_dot_to_svg.mjs <dot-dir> <svg-dir> " +
        "[--viz-module <package-or-file>]"
    );
}


function parseArguments(argv) {
    if (argv.length !== 2 && argv.length !== 4) {
        usage();
        process.exitCode = 2;
        return null;
    }
    const [dotDir, svgDir, option, moduleArg] = argv;
    if (argv.length === 4 && option !== "--viz-module") {
        usage();
        process.exitCode = 2;
        return null;
    }
    return { dotDir, svgDir, moduleArg: moduleArg ?? DEFAULT_VIZ_MODULE };
}


function importSpecifier(value) {
    if (/^[A-Za-z]:[\\/]/.test(value) || value.startsWith("/")) {
        return pathToFileURL(path.resolve(value)).href;
    }
    return value;
}


async function atomicWrite(filePath, payload) {
    const temporaryPath = path.join(
        path.dirname(filePath),
        `.${path.basename(filePath)}.tmp`
    );
    try {
        await writeFile(temporaryPath, payload, { encoding: "utf8" });
        await rename(temporaryPath, filePath);
    } finally {
        await rm(temporaryPath, { force: true });
    }
}


async function main() {
    const args = parseArguments(process.argv.slice(2));
    if (args === null) {
        return;
    }

    const { readdir } = await import("node:fs/promises");
    const entries = (await readdir(args.dotDir, { withFileTypes: true }))
        .filter((entry) => entry.isFile() && entry.name.endsWith(".dot"))
        .map((entry) => entry.name)
        .sort((left, right) => left.localeCompare(right, "en"));
    if (entries.length === 0) {
        throw new Error(`no DOT files found in ${args.dotDir}`);
    }

    const module = await import(importSpecifier(args.moduleArg));
    if (typeof module.instance !== "function") {
        throw new Error("Viz module does not export instance()");
    }
    const viz = await module.instance();
    if (viz.graphvizVersion !== EXPECTED_GRAPHVIZ_VERSION) {
        throw new Error(
            `Graphviz ${EXPECTED_GRAPHVIZ_VERSION} is required for byte-stable output; ` +
            `found ${viz.graphvizVersion}`
        );
    }
    await mkdir(args.svgDir, { recursive: true });

    for (const filename of entries) {
        const dot = await readFile(path.join(args.dotDir, filename), "utf8");
        let svg = viz.renderString(dot, { format: "svg", engine: "dot" });
        svg = svg.replaceAll("\r\n", "\n").replaceAll("\r", "\n");
        if (!svg.includes("<svg") || svg.includes("<script")) {
            throw new Error(`unsafe or malformed SVG generated for ${filename}`);
        }
        const outputName = filename.replace(/\.dot$/, ".svg");
        await atomicWrite(path.join(args.svgDir, outputName), svg.trimEnd() + "\n");
    }

    console.log(
        `wrote ${entries.length} SVG files to ${args.svgDir} ` +
        `(Graphviz ${viz.graphvizVersion})`
    );
}


main().catch((error) => {
    console.error(`error: ${error.message}`);
    process.exitCode = 1;
});
