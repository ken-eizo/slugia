"use strict";

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const childProcess = require("child_process");
const vm = require("vm");

const repository = path.resolve(__dirname, "..");
const pluginPath = path.join(repository, "tools", "figma-slugui", "code.js");
const pluginCode = fs.readFileSync(pluginPath, "utf8");
const manifest = JSON.parse(fs.readFileSync(
  path.join(repository, "tools", "figma-slugui", "manifest.json"), "utf8"));
assert.strictEqual(manifest.documentAccess, "dynamic-page");
assert.deepStrictEqual(manifest.networkAccess.allowedDomains, ["none"]);
const ui = fs.readFileSync(path.join(repository, "tools", "figma-slugui", "ui.html"), "utf8");
const uiScript = ui.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(uiScript, "plugin UI script should exist");
new Function(uiScript[1]);
const messages = [];
const listeners = {};
const pageListeners = {};
const asyncNodeLookup = new Map();
let outlineStrokeCalls = 0;

const solid = (r, g, b, opacity = 1) => ({
  type: "SOLID", visible: true, opacity, color: { r, g, b },
});

function providerDataFor(source, nodeId) {
  const marker = "source-node: " + JSON.stringify(nodeId) + ";";
  const start = source.indexOf(marker);
  assert.ok(start >= 0, "missing source metadata for " + nodeId);
  const match = source.slice(start).match(/source-provider-data: ("(?:\\.|[^"\\])*");/);
  assert.ok(match, "missing provider data for " + nodeId);
  return JSON.parse(JSON.parse(match[1]));
}

const base = (overrides) => Object.assign({
  visible: true,
  opacity: 1,
  x: 0,
  y: 0,
  width: 100,
  height: 40,
  fills: [],
  strokes: [],
  effects: [],
  layoutSizingHorizontal: "FIXED",
  layoutSizingVertical: "FIXED",
  clone() { return Object.assign({}, this, { remove() {} }); },
  outlineStroke() {
    ++outlineStrokeCalls;
    return base({
      type: "VECTOR",
      id: String(this.id || "") + ":outline",
      name: String(this.name || "") + " outline",
      x: this.x, y: this.y, width: this.width, height: this.height,
      fills: this.strokes,
      fillGeometry: this.outlinedStrokeGeometry || this.strokeGeometry || [],
      absoluteBoundingBox: this.outlinedAbsoluteBoundingBox ||
        this.absoluteRenderBounds || this.absoluteBoundingBox,
      remove() {},
    });
  },
  remove() {},
}, overrides);

const selection = [
  base({
    type: "FRAME",
    id: "12:1",
    name: "Settings Panel",
    width: 640,
    height: 420,
    layoutMode: "VERTICAL",
    layoutSizingHorizontal: "FIXED",
    layoutSizingVertical: "FIXED",
    paddingLeft: 24,
    paddingTop: 20,
    paddingRight: 24,
    paddingBottom: 20,
    itemSpacing: 12,
    primaryAxisAlignItems: "MIN",
    counterAxisAlignItems: "MIN",
    clipsContent: true,
    cornerRadius: 16,
    cornerSmoothing: 0.8,
    fills: [solid(0.06, 0.09, 0.16)],
    children: [
      base({
        type: "RECTANGLE",
        id: "12:2",
        name: "Accent Card",
        width: 592,
        height: 90,
        layoutSizingHorizontal: "FILL",
        layoutSizingVertical: "FIXED",
        cornerRadius: Symbol("mixed-corner"),
        topLeftRadius: 4,
        topRightRadius: 8,
        bottomRightRadius: 12,
        bottomLeftRadius: 16,
        cornerSmoothing: 0.5,
        fills: [{
          type: "GRADIENT_LINEAR",
          visible: true,
          opacity: 1,
          gradientTransform: [[1, 0, 0], [0, 1, 0]],
          gradientStops: [
            { position: 0, color: { r: 0.2, g: 0.5, b: 1, a: 1 } },
            { position: 1, color: { r: 0.7, g: 0.3, b: 1, a: 1 } },
          ],
        }],
        strokes: [solid(0.1, 0.9, 0.7)],
        strokeWeight: 2.5,
        strokeAlign: "INSIDE",
      }),
      base({
        type: "TEXT",
        id: "12:3",
        name: "Heading",
        width: 592,
        height: 48,
        layoutSizingHorizontal: "FILL",
        layoutSizingVertical: "FIXED",
        characters: "Figma → SlugUI",
        fontSize: 24,
        fontWeight: 700,
        fontName: { family: "Inter", style: "Bold" },
        textDecoration: "NONE",
        textAlignHorizontal: "CENTER",
        lineHeight: { unit: "PERCENT", value: 125 },
        letterSpacing: { unit: "PIXELS", value: 0.5 },
        paragraphIndent: 0,
        fills: [solid(0.94, 0.97, 1)],
      }),
      base({
        type: "VECTOR",
        id: "12:4",
        name: "Logo Mark",
        x: 120,
        y: 80,
        width: 64,
        height: 64,
        absoluteBoundingBox: { x: 120, y: 80, width: 64, height: 64 },
        absoluteRenderBounds: { x: 119, y: 79, width: 66, height: 66 },
        outlinedAbsoluteBoundingBox: { x: -420, y: 380, width: 66, height: 66 },
        layoutSizingHorizontal: "FIXED",
        layoutSizingVertical: "FIXED",
        fills: [solid(0.2, 0.9, 0.7)],
        fillGeometry: [{
          windingRule: "NONZERO",
          data: "M 2 2 L 62 2 L 32 60 Z",
        }],
        strokes: [solid(1, 0.25, 0.5)],
        strokeWeight: 2,
        strokeAlign: "CENTER",
        strokeGeometry: [{
          windingRule: "NONZERO",
          data: "M 1 1 L 63 1 L 32 62 Z M 4 4 L 32 57 L 60 4 Z",
        }],
      }),
      base({
        type: "VECTOR",
        id: "12:5",
        name: "Inside Stroke",
        x: 210,
        y: 80,
        width: 24,
        height: 24,
        absoluteBoundingBox: { x: 210, y: 80, width: 24, height: 24 },
        absoluteRenderBounds: { x: 210, y: 80, width: 24, height: 24 },
        layoutSizingHorizontal: "FIXED",
        layoutSizingVertical: "FIXED",
        fills: [],
        strokes: [solid(0.4, 0.7, 1)],
        strokeWeight: 2,
        strokeAlign: "INSIDE",
        strokeGeometry: [{
          windingRule: "NONZERO",
          data: "M 1 1 L 23 1 L 23 23 L 1 23 Z M 3 3 L 3 21 L 21 21 L 21 3 Z",
        }],
      }),
    ],
  }),
];
selection[0].children[0].cornerRadius = Symbol.for("figma.mixed");

let flattenCalls = 0;
const figma = {
  mixed: Symbol.for("figma.mixed"),
  fileKey: "fixture-file",
  root: { documentColorProfile: "SRGB" },
  flatten(nodes) { ++flattenCalls; return nodes[0]; },
  getNodeByIdAsync(id) { return Promise.resolve(asyncNodeLookup.get(String(id)) || null); },
  getImageByHash(hash) {
    if (String(hash) !== "fixture-image-1920x1080") return null;
    return {
      getSizeAsync() { return Promise.resolve({ width: 1920, height: 1080 }); },
    };
  },
  currentPage: {
    selection,
    on(event, callback) { pageListeners[event] = callback; },
    off(event, callback) {
      if (pageListeners[event] === callback) delete pageListeners[event];
    },
  },
  ui: {
    postMessage(message) { messages.push(message); },
    onmessage: null,
  },
  showUI(_html, options) {
    assert.strictEqual(options.themeColors, true);
  },
  on(event, callback) { listeners[event] = callback; },
  closePlugin() {},
};
selection[0].children[0].cornerRadius = figma.mixed;

vm.runInNewContext(pluginCode, {
  figma,
  __html__: "<html></html>",
  console,
  setTimeout(callback) { callback(); return 1; },
  clearTimeout() {},
}, { filename: pluginPath });

assert.ok(messages.length > 0, "plugin should post an initial export");
const exported = messages[messages.length - 1];
assert.strictEqual(exported.type, "export-result");
assert.strictEqual(exported.formatVersion, 3);
assert.strictEqual(exported.component, "Settings_PanelGenerated");
assert.match(exported.source, /component Settings_PanelGenerated/);
assert.match(exported.source, /layout: column;/);
assert.match(exported.source, /padding: \[24px, 20px, 24px, 20px\];/);
assert.match(exported.source, /linear\(#3380ff, #b34dff\)/);
assert.match(exported.source, /radius: \[4, 8, 12, 16\];/);
assert.match(exported.source, /stroke: #1ae6b3;/);
assert.match(exported.source, /stroke-width: 2.5;/);
assert.match(exported.source, /stroke-align: inside;/);
assert.match(exported.source, /text: "Figma → SlugUI";/);
assert.match(exported.source, /font-weight: 700;/);
assert.doesNotMatch(exported.source, /bold: true;/);
assert.match(exported.source, /path-data: "M 2 2 L 62 2 L 32 60 Z";/);
assert.match(exported.source, /stroke-path-data:/);
assert.match(exported.source, /path-placement: \[0px, 0px, 64px, 64px\];/);
assert.match(exported.source, /stroke-placement: \[-1px, -1px, 66px, 66px\];/);
assert.doesNotMatch(exported.source, /stroke-placement: \[-(?:1\d\d|[2-9]\d{2,})/);
assert.strictEqual(flattenCalls, 0, "exporting must not reparent vector geometry through flatten");
assert.match(exported.source, /fidelity: baked-vector;/);
assert.strictEqual(exported.fidelityCounts.bakedVector, 2);
assert.strictEqual(exported.fidelityCounts.unsupported, 0);
assert.ok(exported.fidelityCounts.approximated >= 1);
assert.ok(!exported.diagnostics.some((item) => item.code === "E_VECTOR_GEOMETRY_MISSING"));
assert.ok(exported.diagnostics.some((item) => item.code === "W_GRADIENT_TRANSFORM"));
assert.ok(!exported.diagnostics.some((item) => item.code === "W_VECTOR_STROKE_ALIGN"));
assert.ok(!exported.diagnostics.some((item) => item.code === "E_VECTOR_STROKE_OUTLINE"));
assert.ok(pageListeners.nodechange, "current-page node edits should trigger an incremental export refresh");
const initialOutlineCalls = outlineStrokeCalls;
assert.strictEqual(initialOutlineCalls, 1,
  "center-aligned vector strokes should use strokeGeometry without outlineStroke mutation");
figma.ui.onmessage({ type: "refresh" });
const cachedExport = messages[messages.length - 1];
assert.ok(cachedExport.stats.vectorCacheHits >= 2);
assert.strictEqual(outlineStrokeCalls, initialOutlineCalls,
  "manual refresh must reuse exact vector geometry when source geometry is unchanged");

const messageCountBeforeUnrelatedEdit = messages.length;
pageListeners.nodechange({
  nodeChanges: [{ type: "PROPERTY_CHANGE", id: "999:1", properties: ["fills"] }],
});
assert.strictEqual(messages.length, messageCountBeforeUnrelatedEdit,
  "editing an unrelated Figma node must not regenerate the active selection");
pageListeners.nodechange({
  nodeChanges: [{ type: "PROPERTY_CHANGE", id: "12:2", properties: ["fills"] }],
});
assert.strictEqual(messages.length, messageCountBeforeUnrelatedEdit + 1,
  "editing a node in the active export subtree must refresh it");

figma.currentPage.selection = [base({
  type: "LINE",
  id: "20:1",
  name: "Zero Height Divider",
  x: 40,
  y: 80,
  width: 23,
  height: 0,
  absoluteBoundingBox: { x: 40, y: 80, width: 23, height: 0 },
  absoluteRenderBounds: { x: 40, y: 77, width: 23, height: 6 },
  strokes: [solid(0.5, 0.5, 0.5)],
  strokeWeight: 6,
  strokeAlign: "CENTER",
  outlinedStrokeGeometry: [{
    windingRule: "NONZERO",
    data: "M23 0 L23 6 L0 6 L0 0 L23 0 Z",
  }],
})];
figma.ui.onmessage({ type: "refresh" });
const zeroHeightLine = messages[messages.length - 1];
assert.match(zeroHeightLine.source, /width: 23px;/);
assert.match(zeroHeightLine.source, /height: 6px;/);
assert.match(zeroHeightLine.source, /stroke-placement: \[0px, 0px, 23px, 6px\];/);
assert.doesNotMatch(zeroHeightLine.source, /height: 0px;/);

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "slugui-figma-test-"));
const sourcePath = path.join(temporary, exported.filename);
fs.writeFileSync(sourcePath, exported.source, "utf8");
const python = process.env.SLUGUI_PYTHON || (process.platform === "win32" ? "python" : "python3");
const compile = childProcess.spawnSync(python, [
  path.join(repository, "tools", "slugui_compiler.py"),
  sourcePath,
  "--check",
  "--namespace",
  "slugia::figma_fixture",
], { cwd: repository, encoding: "utf8" });
fs.rmSync(temporary, { recursive: true, force: true });
assert.strictEqual(compile.status, 0, compile.stderr || compile.stdout);

figma.currentPage.selection = [base({
  type: "GROUP",
  id: "31:1",
  name: "Absolute Group",
  x: 100,
  y: 50,
  width: 220,
  height: 140,
  absoluteBoundingBox: { x: 100, y: 50, width: 220, height: 140 },
  children: [
    base({
      type: "RECTANGLE",
      id: "31:2",
      name: "Local Card",
      x: 12,
      y: 14,
      width: 80,
      height: 30,
      absoluteBoundingBox: { x: 112, y: 64, width: 80, height: 30 },
      fills: [solid(0.2, 0.4, 0.8)],
    }),
    base({
      type: "GROUP",
      id: "31:3",
      name: "Nested Group",
      x: 40,
      y: 30,
      width: 90,
      height: 40,
      absoluteBoundingBox: { x: 140, y: 80, width: 90, height: 40 },
      children: [base({
        type: "TEXT",
        id: "31:4",
        name: "Nested Text",
        x: 5,
        y: 5,
        width: 70,
        height: 20,
        absoluteBoundingBox: { x: 145, y: 85, width: 70, height: 20 },
        characters: "Nested",
        fontSize: 14,
        fontWeight: 400,
        fontName: { family: "Inter", style: "Regular" },
        textDecoration: "NONE",
        textAlignHorizontal: "LEFT",
        lineHeight: { unit: "AUTO" },
        letterSpacing: { unit: "PIXELS", value: 0 },
        paragraphIndent: 0,
        fills: [solid(1, 1, 1)],
      })],
    }),
  ],
})];
figma.ui.onmessage({ type: "refresh" });
const grouped = messages[messages.length - 1];
assert.match(grouped.source, /Rectangle Local_Card_31_2 \{\n\s+x: 12px;\n\s+y: 14px;\n\s+width: 80px;\n\s+height: 30px;/);
assert.match(grouped.source, /Absolute Nested_Group_31_3 \{\n\s+x: 40px;\n\s+y: 30px;\n\s+width: 90px;\n\s+height: 40px;/);
assert.match(grouped.source, /Text Nested_Text_31_4 \{\n\s+x: 5px;\n\s+y: 5px;\n\s+width: 70px;\n\s+height: 20px;/);

figma.currentPage.selection = [base({
  type: "RECTANGLE",
  id: "40:1",
  name: "Per Side Border",
  width: 120,
  height: 50,
  cornerRadius: 8,
  fills: [solid(0.1, 0.1, 0.1)],
  strokes: [solid(0.9, 0.4, 0.2)],
  strokeWeight: figma.mixed,
  strokeTopWeight: 1,
  strokeRightWeight: 2,
  strokeBottomWeight: 3,
  strokeLeftWeight: 4,
  strokeAlign: "OUTSIDE",
})];
figma.ui.onmessage({ type: "refresh" });
const perSide = messages[messages.length - 1];
assert.match(perSide.source, /stroke-width: \[1, 2, 3, 4\];/);
assert.match(perSide.source, /stroke-align: outside;/);
assert.ok(!perSide.diagnostics.some((item) => item.code === "W_INDIVIDUAL_STROKE_WIDTH"));

const multipleNodes = [
  base({
    type: "GROUP", id: "50:1", name: "Left Group",
    x: 100, y: 40, width: 80, height: 60,
    absoluteBoundingBox: { x: 100, y: 40, width: 80, height: 60 },
    children: [base({
      type: "RECTANGLE", id: "50:2", name: "Left Card",
      x: 100, y: 40, width: 80, height: 60,
      absoluteBoundingBox: { x: 100, y: 40, width: 80, height: 60 },
      fills: [solid(1, 0, 0)],
    })],
  }),
  base({
    type: "GROUP", id: "51:1", name: "Right Group",
    x: 240, y: 70, width: 100, height: 50,
    absoluteBoundingBox: { x: 240, y: 70, width: 100, height: 50 },
    children: [base({
      type: "RECTANGLE", id: "51:2", name: "Right Card",
      x: 240, y: 70, width: 100, height: 50,
      absoluteBoundingBox: { x: 240, y: 70, width: 100, height: 50 },
      fills: [solid(0, 0, 1)],
    })],
  }),
];
const painterParent = { children: multipleNodes };
for (const node of multipleNodes) node.parent = painterParent;
// Selection order is intentionally reversed; exporter must recover document painter order.
figma.currentPage.selection = [multipleNodes[1], multipleNodes[0]];
figma.ui.onmessage({ type: "refresh" });
const multiple = messages[messages.length - 1];
assert.strictEqual(multiple.component, "FigmaSelectionGenerated");
assert.match(multiple.source, /^\/\/ SlugUI exchange format 3,/);
assert.match(multiple.source, /Absolute selection_root \{\n    width: 240px;\n    height: 80px;/);
assert.match(multiple.source, /Absolute Left_Group_50_1 \{\n      x: 0px;\n      y: 0px;/);
assert.match(multiple.source, /Absolute Right_Group_51_1 \{\n      x: 140px;\n      y: 30px;/);
assert.match(multiple.source, /Left_Card_50_2/);
assert.match(multiple.source, /Right_Card_51_2/);
assert.ok(multiple.source.indexOf("Left_Group_50_1") <
          multiple.source.indexOf("Right_Group_51_1"));
const multiDirectory = fs.mkdtempSync(path.join(os.tmpdir(), "slugui-figma-multi-"));
const multiPath = path.join(multiDirectory, multiple.filename);
fs.writeFileSync(multiPath, multiple.source, "utf8");
const multiCompile = childProcess.spawnSync(python, [
  path.join(repository, "tools", "slugui_compiler.py"),
  multiPath, "--check", "--class-name", "ImportedFigmaPanel",
], { cwd: repository, encoding: "utf8" });
fs.rmSync(multiDirectory, { recursive: true, force: true });
assert.strictEqual(multiCompile.status, 0, multiCompile.stderr || multiCompile.stdout);

const manyNodes = Array.from({ length: 48 }, (_, index) => base({
  type: "RECTANGLE",
  id: "60:" + index,
  name: "Cell " + index,
  x: 100 + (index % 12) * 20,
  y: 50 + Math.floor(index / 12) * 20,
  width: 10,
  height: 10,
  absoluteBoundingBox: {
    x: 100 + (index % 12) * 20,
    y: 50 + Math.floor(index / 12) * 20,
    width: 10,
    height: 10,
  },
  fills: [solid(index / 48, 0.4, 0.8)],
}));
const manyParent = { children: manyNodes, parent: null };
for (const node of manyNodes) node.parent = manyParent;
figma.currentPage.selection = manyNodes.slice().reverse();
figma.ui.onmessage({ type: "refresh" });
const many = messages[messages.length - 1];
assert.strictEqual(many.selectionCount, 48);
assert.match(many.source, /Absolute selection_root \{\n    width: 230px;\n    height: 70px;/);
assert.strictEqual((many.source.match(/Rectangle Cell_/g) || []).length, 48);

const ancestor = base({
  type: "GROUP",
  id: "70:1",
  name: "Selected Parent",
  x: 10,
  y: 10,
  width: 100,
  height: 100,
  absoluteBoundingBox: { x: 10, y: 10, width: 100, height: 100 },
  children: [base({
    type: "RECTANGLE",
    id: "70:2",
    name: "Selected Child",
    x: 20,
    y: 20,
    width: 20,
    height: 20,
    absoluteBoundingBox: { x: 20, y: 20, width: 20, height: 20 },
    fills: [solid(1, 0, 0)],
  })],
});
ancestor.children[0].parent = ancestor;
figma.currentPage.selection = [ancestor.children[0], ancestor];
figma.ui.onmessage({ type: "refresh" });
const nestedSelection = messages[messages.length - 1];
assert.strictEqual((nestedSelection.source.match(/Rectangle Selected_Child_/g) || []).length, 1);

let styledSegmentCalls = 0;
figma.currentPage.selection = [base({
  type: "TEXT",
  id: "80:1",
  name: "Mixed Style Label",
  width: 180,
  height: 30,
  characters: "Red blue",
  fontName: figma.mixed,
  fontSize: figma.mixed,
  fontWeight: figma.mixed,
  textDecoration: figma.mixed,
  fills: figma.mixed,
  textAlignHorizontal: "CENTER",
  textAlignVertical: "BOTTOM",
  listOptions: { type: "UNORDERED" },
  getStyledTextSegments(fields) {
    ++styledSegmentCalls;
    assert.ok(fields.includes("fills"));
    assert.ok(fields.includes("fontWeight"));
    return [
      {
        characters: "Red",
        start: 0,
        end: 3,
        fontSize: 16,
        fontName: { family: "Geist", style: "Bold" },
        fontWeight: 700,
        fontStyle: "REGULAR",
        textDecoration: "UNDERLINE",
        textCase: "UPPER",
        lineHeight: { unit: "PERCENT", value: 125 },
        letterSpacing: { unit: "PIXELS", value: 0 },
        fills: [solid(1, 0, 0)],
      },
      {
        characters: " blue",
        start: 3,
        end: 8,
        fontSize: 14,
        fontName: { family: "Geist", style: "Regular Italic" },
        fontWeight: 400,
        fontStyle: "ITALIC",
        textDecoration: "NONE",
        textCase: "ORIGINAL",
        lineHeight: { unit: "PERCENT", value: 140 },
        letterSpacing: { unit: "PIXELS", value: 0.5 },
        fills: [solid(0, 0, 1)],
      },
    ];
  },
})];
figma.ui.onmessage({ type: "refresh" });
const richText = messages[messages.length - 1];
assert.match(richText.source, /text-align-vertical: bottom;/);
assert.match(richText.source, /list-marker: bullet;/);
assert.match(richText.source, /text: "RED blue";/);
assert.match(richText.source, /text-runs: \[text-run\("RED", "Geist", 16, 700, false, true, false, 0, 1\.25, #ff0000, "Bold"\), text-run\(" blue", "Geist", 14, 400, true, false, false, 0\.5, 1\.4, #0000ff, "Regular Italic"\)\];/);
assert.doesNotMatch(richText.source, /W_MIXED_FILL/);
assert.ok(!richText.diagnostics.some((item) => item.code.startsWith("W_MIXED_")),
  JSON.stringify(richText.diagnostics));
assert.strictEqual(styledSegmentCalls, 1);
figma.ui.onmessage({ type: "refresh" });
const cachedRichText = messages[messages.length - 1];
assert.ok(cachedRichText.stats.textCacheHits >= 1);
assert.strictEqual(styledSegmentCalls, 1,
  "manual refresh must reuse styled text segments while the text node is unchanged");

const richDirectory = fs.mkdtempSync(path.join(os.tmpdir(), "slugui-figma-rich-"));
const richPath = path.join(richDirectory, richText.filename);
fs.writeFileSync(richPath, richText.source, "utf8");
const richCompile = childProcess.spawnSync(python, [
  path.join(repository, "tools", "slugui_compiler.py"),
  richPath, "--check", "--namespace", "slugia::figma_fixture",
], { cwd: repository, encoding: "utf8" });
fs.rmSync(richDirectory, { recursive: true, force: true });
assert.strictEqual(richCompile.status, 0, richCompile.stderr || richCompile.stdout);

const componentDependencyChild = base({
  type: "RECTANGLE", id: "90:4", name: "Component Dependency Child",
  width: 16, height: 16,
  absoluteBoundingBox: { x: 12, y: 12, width: 16, height: 16 },
  componentPropertyReferences: { visible: "Disabled" },
  fills: [solid(0.3, 0.6, 0.9)],
});
const slotChild = base({
  type: "SLOT", id: "90:5", name: "Content Slot",
  x: 36, y: 8, width: 72, height: 20,
  absoluteBoundingBox: { x: 36, y: 8, width: 72, height: 20 },
  componentPropertyReferences: { slotContentId: "ContentSlot" },
  children: [],
});
const componentLabelChild = base({
  type: "TEXT", id: "90:8", name: "Component Label",
  x: 36, y: 8, width: 72, height: 20,
  absoluteBoundingBox: { x: 36, y: 8, width: 72, height: 20 },
  componentPropertyReferences: { characters: "Label" },
  characters: "Label",
  fontSize: 12,
  fontWeight: 400,
  fontName: { family: "Inter", style: "Regular" },
  fills: [solid(1, 1, 1)],
});
const component = base({
  type: "COMPONENT", id: "90:2", name: "Check / On", key: "component-key",
  x: 10, y: 10, width: 120, height: 32,
  absoluteBoundingBox: { x: 10, y: 10, width: 120, height: 32 },
  variantProperties: { State: "On", Tone: "Blue" },
  children: [componentDependencyChild, slotChild, componentLabelChild],
});
componentDependencyChild.parent = component;
slotChild.parent = component;
componentLabelChild.parent = component;

const instanceDependencyChild = base({
  type: "RECTANGLE", id: "90:3:4", name: "Instance Dependency Child",
  x: 12, y: 56, width: 16, height: 16,
  absoluteBoundingBox: { x: 12, y: 56, width: 16, height: 16 },
  componentPropertyReferences: { visible: "Disabled" },
  visible: false,
  fills: [solid(0.3, 0.6, 0.9)],
});
const instanceLabelChild = base({
  type: "TEXT", id: "90:3:8", name: "Instance Label",
  x: 36, y: 58, width: 72, height: 20,
  absoluteBoundingBox: { x: 36, y: 58, width: 72, height: 20 },
  componentPropertyReferences: { characters: "Label" },
  characters: "Ready",
  fontSize: 12,
  fontWeight: 400,
  fontName: { family: "Inter", style: "Regular" },
  fills: [solid(1, 1, 1)],
});
const instance = base({
  type: "INSTANCE", id: "90:3", name: "Check Instance",
  x: 10, y: 52, width: 120, height: 32,
  absoluteBoundingBox: { x: 10, y: 52, width: 120, height: 32 },
  mainComponent: component,
  variantProperties: { State: "On", Tone: "Blue" },
  componentProperties: {
    State: { type: "VARIANT", value: "On" },
    Tone: { type: "VARIANT", value: "Blue" },
    Label: { type: "TEXT", value: "Ready" },
    Disabled: { type: "BOOLEAN", value: false },
  },
  overrides: [{ id: "90:4", overriddenFields: ["fills", "visible"] }],
  exposedInstances: [{ id: "90:nested" }],
  isExposedInstance: true,
  scaleFactor: 1.25,
  children: [instanceDependencyChild, instanceLabelChild],
});
instanceDependencyChild.parent = instance;
instanceLabelChild.parent = instance;
const alternateVariantChild = base({
  type: "RECTANGLE", id: "90:7", name: "Alternate Variant Child",
  width: 16, height: 16,
  absoluteBoundingBox: { x: 12, y: 60, width: 16, height: 16 },
  fills: [solid(0.8, 0.4, 0.2)],
});
const alternateComponent = base({
  type: "COMPONENT", id: "90:6", name: "Check / Off", key: "component-key-off",
  x: 10, y: 54, width: 120, height: 32,
  absoluteBoundingBox: { x: 10, y: 54, width: 120, height: 32 },
  variantProperties: { State: "Off", Tone: "Blue" },
  children: [alternateVariantChild],
});
alternateVariantChild.parent = alternateComponent;
const componentSet = base({
  type: "COMPONENT_SET", id: "90:1", name: "Checkboxes",
  x: 0, y: 0, width: 140, height: 96,
  absoluteBoundingBox: { x: 0, y: 0, width: 140, height: 96 },
  componentPropertyDefinitions: {
    State: { type: "VARIANT", defaultValue: "On", variantOptions: ["On", "Off"] },
    Tone: { type: "VARIANT", defaultValue: "Blue", variantOptions: ["Blue"] },
    Label: { type: "TEXT", defaultValue: "Label" },
    Disabled: { type: "BOOLEAN", defaultValue: false },
    ContentSlot: { type: "SLOT", defaultValue: "", slotSettings: {} },
    Icon: {
      type: "INSTANCE_SWAP", defaultValue: "90:icon",
      preferredValues: [
        { type: "COMPONENT", key: "preferred-a" },
        { type: "COMPONENT", key: "preferred-b" },
        { type: "COMPONENT", key: "preferred-c" },
      ],
    },
  },
  children: [component, alternateComponent],
});
component.parent = componentSet;
alternateComponent.parent = componentSet;
const instanceHost = { type: "FRAME", id: "90:host", children: [instance], parent: null };
instance.parent = instanceHost;

figma.currentPage.selection = [componentSet];
figma.ui.onmessage({ type: "refresh" });
const componentExport = messages[messages.length - 1];
const componentData = providerDataFor(componentExport.source, "90:2");
const componentChildData = providerDataFor(componentExport.source, "90:4");
const slotData = providerDataFor(componentExport.source, "90:5");
assert.deepStrictEqual(componentChildData.componentPropertyReferences, { visible: "Disabled" });
assert.strictEqual(slotData.nodeType, "SLOT");
assert.deepStrictEqual(slotData.componentPropertyReferences, { slotContentId: "ContentSlot" });
const disabledProperty = componentExport.source.match(
  /property bool (figma_Disabled_[0-9a-f]{8}) = false;/);
assert.ok(disabledProperty, "BOOLEAN component properties must become typed SlugUI properties");
assert.ok(componentExport.source.includes("visible: $" + disabledProperty[1] + ";"),
  "Figma visible component-property references must remain live bindings");
const labelProperty = componentExport.source.match(
  /property string (figma_Label_[0-9a-f]{8}) = "Label";/);
assert.ok(labelProperty, "TEXT component properties must become typed SlugUI properties");
assert.ok(componentExport.source.includes("text: $" + labelProperty[1] + ";"),
  "Figma text component-property references must remain live bindings");
const stateProperty = componentExport.source.match(
  /property string (figma_State_[0-9a-f]{8}) = "On";/);
assert.ok(stateProperty,
  "ComponentSet VARIANT properties must be exported as one typed runtime property");
assert.strictEqual((componentExport.source.match(/property string figma_State_[0-9a-f]{8}/g) || []).length, 1,
  "ComponentSet runtime property scope must deduplicate the same VARIANT across variants");
assert.ok(!componentExport.diagnostics.some(
  (item) => item.nodeId === "90:5" && item.code === "E_NODE_TYPE"));

figma.currentPage.selection = [instance];
figma.ui.onmessage({ type: "refresh" });
const instanceExport = messages[messages.length - 1];
const instanceData = providerDataFor(instanceExport.source, "90:3");
assert.strictEqual(componentData.nodeType, "COMPONENT");
assert.strictEqual(componentData.componentKey, "component-key");
assert.strictEqual(componentData.componentSetId, "90:1");
assert.deepStrictEqual(componentData.variantProperties, { State: "On", Tone: "Blue" });
assert.strictEqual(componentData.componentDefinitions.Label.defaultValue, "Label");
assert.strictEqual(componentData.componentDefinitions.Icon.preferredValueCount, 3);
assert.strictEqual(componentData.componentDefinitions.Icon.preferredValues, undefined,
  "instance-swap discovery candidates must not be duplicated into every exported node");
assert.strictEqual(instanceData.nodeType, "INSTANCE");
assert.strictEqual(instanceData.mainComponentId, "90:2");
assert.strictEqual(instanceData.componentSetId, "90:1");
assert.strictEqual(instanceData.componentKey, "component-key");
assert.strictEqual(instanceData.variantProperties, undefined,
  "Instance variantProperties is deprecated and should not be read by the exporter");
assert.strictEqual(instanceData.componentProperties.State.value, "On");
assert.strictEqual(instanceData.componentProperties.Tone.value, "Blue");
assert.deepStrictEqual(instanceData.variantSelection, { State: "On", Tone: "Blue" });
assert.strictEqual(instanceData.componentProperties.Label.value, "Ready");
assert.strictEqual(instanceData.componentDefinitions.Label.defaultValue, "Label");
assert.match(instanceExport.source, /property string figma_State_[0-9a-f]{8} = "On";/);
assert.match(instanceExport.source, /property string figma_Tone_[0-9a-f]{8} = "Blue";/);
const instanceLabelProperty = instanceExport.source.match(
  /property string (figma_Label_[0-9a-f]{8}) = "Ready";/);
const instanceDisabledProperty = instanceExport.source.match(
  /property bool (figma_Disabled_[0-9a-f]{8}) = false;/);
assert.ok(instanceLabelProperty && instanceDisabledProperty);
assert.ok(instanceExport.source.includes("text: $" + instanceLabelProperty[1] + ";"),
  "resolved instance text must bind to the instance-local TEXT property");
assert.ok(instanceExport.source.includes("visible: $" + instanceDisabledProperty[1] + ";"),
  "hidden resolved instance children must stay exported when visibility is property-bound");
assert.deepStrictEqual(instanceData.overrides,
  [{ id: "90:4", overriddenFields: ["fills", "visible"] }]);
assert.deepStrictEqual(instanceData.exposedInstanceIds, ["90:nested"]);
assert.strictEqual(instanceData.isExposedInstance, true);
assert.strictEqual(instanceData.scaleFactor, 1.25);

// Instance synchronization tracks the main component's descendants, but ignores unrelated
// document edits so a large selection is not continuously regenerated.
const beforeUnrelatedEdit = messages.length;
pageListeners.nodechange({
  nodeChanges: [{ type: "PROPERTY_CHANGE", id: "unrelated:1", properties: ["fills"] }],
});
assert.strictEqual(messages.length, beforeUnrelatedEdit,
  "unrelated document edits must not regenerate the selected instance");
pageListeners.nodechange({
  nodeChanges: [{ type: "PROPERTY_CHANGE", id: "90:7", properties: ["fills"] }],
});
assert.strictEqual(messages.length, beforeUnrelatedEdit,
  "inactive component variants must not regenerate the selected instance");
pageListeners.nodechange({
  nodeChanges: [{ type: "PROPERTY_CHANGE", id: "90:4", properties: ["fills"] }],
});
assert.strictEqual(messages.length, beforeUnrelatedEdit + 1,
  "main-component descendant edits must refresh the selected instance");

const autoLayoutFrame = base({
  type: "FRAME", id: "95:1", name: "Auto Layout With Overlay",
  x: 100, y: 200, width: 200, height: 80,
  absoluteBoundingBox: { x: 100, y: 200, width: 200, height: 80 },
  layoutMode: "HORIZONTAL",
  itemSpacing: 10,
  itemReverseZIndex: true,
  primaryAxisAlignItems: "MIN",
  counterAxisAlignItems: "MIN",
  children: [
    base({
      type: "RECTANGLE", id: "95:2", name: "Flow Fill",
      x: 100, y: 200, width: 50, height: 80,
      absoluteBoundingBox: { x: 100, y: 200, width: 50, height: 80 },
      layoutSizingHorizontal: "FIXED",
      layoutSizingVertical: "FILL",
      fills: [solid(0.2, 0.3, 0.4)],
    }),
    base({
      type: "RECTANGLE", id: "95:3", name: "Floating Badge",
      x: 150, y: 212, width: 30, height: 20,
      absoluteBoundingBox: { x: 150, y: 212, width: 30, height: 20 },
      layoutPositioning: "ABSOLUTE",
      layoutSizingHorizontal: "FILL",
      layoutSizingVertical: "FILL",
      minWidth: 20,
      maxWidth: 40,
      fills: [solid(0.8, 0.2, 0.3)],
    }),
  ],
});
for (const child of autoLayoutFrame.children) child.parent = autoLayoutFrame;
figma.currentPage.selection = [autoLayoutFrame];
figma.ui.onmessage({ type: "refresh" });
const autoLayoutExport = messages[messages.length - 1];
assert.match(autoLayoutExport.source,
  /Rectangle Flow_Fill_95_2 \{\n\s+width: 50px;\n\s+align-self: stretch;/);
assert.match(autoLayoutExport.source,
  /Rectangle Floating_Badge_95_3 \{\n\s+x: 50px;\n\s+y: 12px;\n\s+position: absolute;\n\s+width: 30px;\n\s+height: 20px;/);
assert.match(autoLayoutExport.source, /min-width: 20px;/);
assert.match(autoLayoutExport.source, /max-width: 40px;/);
assert.match(autoLayoutExport.source, /reverse-paint-order: true;/);
assert.strictEqual((autoLayoutExport.source.match(/reverse-paint-order:/g) || []).length, 1,
  "reverse paint order must be emitted once so the AOT parser does not see a duplicate attribute");
const autoLayoutDirectory = fs.mkdtempSync(path.join(os.tmpdir(), "slugui-figma-layout-"));
const autoLayoutPath = path.join(autoLayoutDirectory, autoLayoutExport.filename);
fs.writeFileSync(autoLayoutPath, autoLayoutExport.source, "utf8");
const autoLayoutCompile = childProcess.spawnSync(python, [
  path.join(repository, "tools", "slugui_compiler.py"),
  autoLayoutPath, "--check", "--namespace", "slugia::figma_fixture",
], { cwd: repository, encoding: "utf8" });
fs.rmSync(autoLayoutDirectory, { recursive: true, force: true });
assert.strictEqual(autoLayoutCompile.status, 0, autoLayoutCompile.stderr || autoLayoutCompile.stdout);

const wrappedFrame = base({
  type: "FRAME", id: "95:10", name: "Wrapped Hug Layout",
  x: 0, y: 0, width: 120, height: 70,
  absoluteBoundingBox: { x: 0, y: 0, width: 120, height: 70 },
  layoutMode: "HORIZONTAL",
  layoutWrap: "WRAP",
  itemSpacing: 8,
  counterAxisSpacing: 12,
  counterAxisAlignContent: "SPACE_BETWEEN",
  primaryAxisAlignItems: "SPACE_EVENLY",
  counterAxisAlignItems: "CENTER",
  children: [
    base({
      type: "TEXT", id: "95:11", name: "Hug Label",
      x: 0, y: 0, width: 46, height: 20,
      absoluteBoundingBox: { x: 0, y: 0, width: 46, height: 20 },
      layoutSizingHorizontal: "HUG",
      layoutSizingVertical: "HUG",
      characters: "Hug me", fontSize: 14, fontWeight: 400,
      fontName: { family: "Inter", style: "Regular" },
      fills: [solid(1, 1, 1)],
    }),
    base({
      type: "RECTANGLE", id: "95:12", name: "Fallback Fill",
      x: 54, y: 0, width: 40, height: 20,
      absoluteBoundingBox: { x: 54, y: 0, width: 40, height: 20 },
      layoutSizingHorizontal: undefined,
      layoutSizingVertical: undefined,
      layoutGrow: 1,
      layoutAlign: "STRETCH",
      fills: [solid(0.2, 0.5, 0.9)],
    }),
  ],
});
for (const child of wrappedFrame.children) child.parent = wrappedFrame;
figma.currentPage.selection = [wrappedFrame];
figma.ui.onmessage({ type: "refresh" });
const wrappedExport = messages[messages.length - 1];
assert.match(wrappedExport.source, /wrap: true;/);
assert.match(wrappedExport.source, /counter-spacing: 12px;/);
assert.match(wrappedExport.source, /counter-justify: space-between;/);
assert.match(wrappedExport.source, /justify: space-evenly;/);
assert.match(wrappedExport.source,
  /Text Hug_Label_95_11 \{\n\s+preferred-width: 46px;\n\s+preferred-height: 20px;/);
assert.match(wrappedExport.source,
  /Rectangle Fallback_Fill_95_12 \{\n\s+grow: 1;\n\s+align-self: stretch;/);


// Snapshot fallback must pass the exported layout contract to children. BASELINE cannot be
// silently treated as MIN because that shifts text-heavy component rows.
const baselineFrame = base({
  type: "FRAME", id: "95:20", name: "Baseline Snapshot",
  x: 300, y: 400, width: 200, height: 80,
  absoluteBoundingBox: { x: 300, y: 400, width: 200, height: 80 },
  layoutMode: "HORIZONTAL",
  counterAxisAlignItems: "BASELINE",
  fills: [solid(0.05, 0.05, 0.05)],
  children: [
    base({
      type: "TEXT", id: "95:21", name: "Baseline Label",
      x: 999, y: 999, width: 60, height: 20,
      absoluteBoundingBox: { x: 310, y: 415, width: 60, height: 20 },
      characters: "Base", fontSize: 14, fontWeight: 400,
      fontName: { family: "Inter", style: "Regular" },
      fills: [solid(1, 1, 1)],
    }),
  ],
});
baselineFrame.children[0].parent = baselineFrame;
figma.currentPage.selection = [baselineFrame];
figma.ui.onmessage({ type: "refresh" });
const baselineExport = messages[messages.length - 1];
assert.match(baselineExport.source, /layout: absolute;/);
assert.match(baselineExport.source,
  /Text Baseline_Label_95_21 \{\n\s+x: 10px;\n\s+y: 15px;/);
assert.ok(baselineExport.diagnostics.some((item) => item.code === "W_BASELINE_LAYOUT"));

// Figma relativeTransform is container-parent based across Group/Boolean nodes. Absolute bounds
// differences are therefore the canonical snapshot coordinates for a direct Group child.
const coordinateGroupChild = base({
  type: "RECTANGLE", id: "95:31", name: "Grouped Child",
  x: 900, y: 700, width: 30, height: 20,
  absoluteBoundingBox: { x: 120, y: 230, width: 30, height: 20 },
  constraints: { horizontal: "MAX", vertical: "CENTER" },
  fills: [solid(0.5, 0.4, 0.3)],
});
const coordinateGroup = base({
  type: "GROUP", id: "95:30", name: "Coordinate Group",
  x: 1, y: 2, width: 100, height: 80,
  absoluteBoundingBox: { x: 100, y: 200, width: 100, height: 80 },
  children: [coordinateGroupChild],
});
coordinateGroupChild.parent = coordinateGroup;
figma.currentPage.selection = [coordinateGroup];
figma.ui.onmessage({ type: "refresh" });
const coordinateGroupExport = messages[messages.length - 1];
assert.match(coordinateGroupExport.source,
  /Rectangle Grouped_Child_95_31 \{\n\s+x: 20px;\n\s+y: 30px;/);
assert.match(coordinateGroupExport.source, /constraint-horizontal: max;/);
assert.match(coordinateGroupExport.source, /constraint-vertical: center;/);
const coordinateGroupData = providerDataFor(coordinateGroupExport.source, "95:31");
assert.deepStrictEqual(coordinateGroupData.constraints, { horizontal: "MAX", vertical: "CENTER" });

// Group synchronization must watch hidden descendants and structural changes without reacting
// to unrelated CREATE/DELETE churn elsewhere on the current page.
const hiddenChild = base({
  type: "RECTANGLE", id: "96:2", name: "Initially Hidden",
  visible: false, x: 10, y: 10, width: 30, height: 20,
  absoluteBoundingBox: { x: 10, y: 10, width: 30, height: 20 },
  fills: [solid(0.2, 0.8, 0.3)],
});
const watchedGroup = base({
  type: "GROUP", id: "96:1", name: "Watched Group",
  x: 0, y: 0, width: 100, height: 80,
  absoluteBoundingBox: { x: 0, y: 0, width: 100, height: 80 },
  children: [hiddenChild],
});
hiddenChild.parent = watchedGroup;
figma.currentPage.selection = [watchedGroup];
figma.ui.onmessage({ type: "refresh" });
const hiddenExport = messages[messages.length - 1];
assert.doesNotMatch(hiddenExport.source, /Initially_Hidden_96_2/);

const beforeHiddenReveal = messages.length;
hiddenChild.visible = true;
pageListeners.nodechange({
  nodeChanges: [{
    type: "PROPERTY_CHANGE", id: "96:2", node: hiddenChild, properties: ["visible"],
  }],
});
assert.strictEqual(messages.length, beforeHiddenReveal + 1);
assert.match(messages[messages.length - 1].source, /Initially_Hidden_96_2/);

const beforeUnrelatedCreate = messages.length;
const unrelatedParent = { type: "GROUP", id: "unrelated-parent", children: [] };
const unrelatedCreated = base({ type: "RECTANGLE", id: "unrelated-created", parent: unrelatedParent });
pageListeners.nodechange({
  nodeChanges: [{ type: "CREATE", id: "unrelated-created", node: unrelatedCreated }],
});
assert.strictEqual(messages.length, beforeUnrelatedCreate,
  "unrelated structural edits must not regenerate the selected group");

const createdChild = base({
  type: "RECTANGLE", id: "96:3", name: "Created Child",
  x: 45, y: 10, width: 30, height: 20,
  absoluteBoundingBox: { x: 45, y: 10, width: 30, height: 20 },
  fills: [solid(0.8, 0.4, 0.2)],
});
createdChild.parent = watchedGroup;
watchedGroup.children.push(createdChild);
const beforeSelectedCreate = messages.length;
pageListeners.nodechange({
  nodeChanges: [{ type: "CREATE", id: "96:3", node: createdChild }],
});
assert.strictEqual(messages.length, beforeSelectedCreate + 1);
assert.match(messages[messages.length - 1].source, /Created_Child_96_3/);

figma.currentPage.selection = [];
figma.ui.onmessage({ type: "refresh" });
const empty = messages[messages.length - 1];
assert.strictEqual(empty.source, "");
assert.ok(empty.diagnostics.some((item) => item.code === "E_NO_SELECTION"));

// dynamic-page Figma deprecates InstanceNode.mainComponent in favor of getMainComponentAsync().
// Verify an instance whose synchronous getter is unavailable still exports component identity.
const asyncInstance = base({
  type: "INSTANCE", id: "91:3", name: "Async Component Instance",
  width: 120, height: 32,
  absoluteBoundingBox: { x: 0, y: 0, width: 120, height: 32 },
  componentProperties: { Label: { type: "TEXT", value: "Async" } },
  children: [],
  getMainComponentAsync() { return Promise.resolve(component); },
});
Object.defineProperty(asyncInstance, "mainComponent", {
  get() { throw new Error("dynamic-page synchronous mainComponent access is unavailable"); },
});
asyncInstance.parent = { type: "FRAME", id: "91:host", children: [asyncInstance], parent: null };
const beforeAsyncComponent = messages.length;
figma.currentPage.selection = [asyncInstance];
figma.ui.onmessage({ type: "refresh" });
assert.strictEqual(messages.length, beforeAsyncComponent,
  "async main-component resolution should not emit incomplete provenance");

setImmediate(() => {
  const asyncExport = messages[messages.length - 1];
  const asyncData = providerDataFor(asyncExport.source, "91:3");
  assert.strictEqual(asyncData.mainComponentId, "90:2");
  assert.strictEqual(asyncData.componentKey, "component-key");

  // A selected instance may itself contain nested instances. All of them must be primed through
  // the dynamic-page async API or nested component swaps/definition edits lose synchronization.
  const nestedAsyncInstance = base({
    type: "INSTANCE", id: "91:5", name: "Nested Async Instance",
    width: 80, height: 24,
    absoluteBoundingBox: { x: 8, y: 8, width: 80, height: 24 },
    children: [],
    getMainComponentAsync() { return Promise.resolve(component); },
  });
  Object.defineProperty(nestedAsyncInstance, "mainComponent", {
    get() { throw new Error("nested dynamic-page mainComponent requires async access"); },
  });
  const outerInstance = base({
    type: "INSTANCE", id: "91:4", name: "Outer Instance",
    width: 120, height: 48,
    absoluteBoundingBox: { x: 0, y: 0, width: 120, height: 48 },
    mainComponent: component,
    children: [nestedAsyncInstance],
  });
  nestedAsyncInstance.parent = outerInstance;
  outerInstance.parent = { type: "FRAME", id: "91:outer-host", children: [outerInstance], parent: null };
  figma.currentPage.selection = [outerInstance];
  const beforeNestedAsync = messages.length;
  figma.ui.onmessage({ type: "refresh" });
  assert.strictEqual(messages.length, beforeNestedAsync,
    "nested async instances should delay export until all component identities are resolved");

  setImmediate(() => {
    const nestedExport = messages[messages.length - 1];
    const nestedData = providerDataFor(nestedExport.source, "91:5");
    assert.strictEqual(nestedData.mainComponentId, "90:2");
    assert.strictEqual(nestedData.componentKey, "component-key");

    // Some Figma structural events expose only the created id. Resolve it lazily and inspect its
    // ancestor chain so additions to a selected group still regenerate without global refreshes.
    const idOnlyGroup = base({
      type: "GROUP", id: "97:1", name: "ID Only Group",
      width: 100, height: 80,
      absoluteBoundingBox: { x: 0, y: 0, width: 100, height: 80 },
      children: [],
    });
    figma.currentPage.selection = [idOnlyGroup];
    figma.ui.onmessage({ type: "refresh" });
    const idOnlyChild = base({
      type: "RECTANGLE", id: "97:2", name: "ID Only Child",
      x: 12, y: 10, width: 40, height: 20,
      absoluteBoundingBox: { x: 12, y: 10, width: 40, height: 20 },
      fills: [solid(0.3, 0.5, 0.9)],
    });
    idOnlyChild.parent = idOnlyGroup;
    idOnlyGroup.children.push(idOnlyChild);
    asyncNodeLookup.set(idOnlyChild.id, idOnlyChild);
    const beforeIdOnlyCreate = messages.length;
    pageListeners.nodechange({ nodeChanges: [{ type: "CREATE", id: idOnlyChild.id }] });
    assert.strictEqual(messages.length, beforeIdOnlyCreate,
      "id-only CREATE resolution is asynchronous");

    setImmediate(() => {
      assert.strictEqual(messages.length, beforeIdOnlyCreate + 1);
      assert.match(messages[messages.length - 1].source, /ID_Only_Child_97_2/);

      // Image fills retain source pixel dimensions and placement semantics even while the
      // current renderer uses a visible geometry-preserving placeholder instead of the bitmap.
      const imageNode = base({
        type: "RECTANGLE", id: "98:image", name: "Hero Image",
        width: 320, height: 180,
        absoluteBoundingBox: { x: 0, y: 0, width: 320, height: 180 },
        fills: [{
          type: "IMAGE", visible: true, opacity: 0.8,
          imageHash: "fixture-image-1920x1080", scaleMode: "FIT",
          scalingFactor: 1,
          rotation: 0,
          imageTransform: [[1, 0, 0], [0, 1, 0]],
        }],
      });
      const beforeImage = messages.length;
      figma.currentPage.selection = [imageNode];
      figma.ui.onmessage({ type: "refresh" });
      assert.strictEqual(messages.length, beforeImage,
        "image-size resolution is asynchronous");

      setImmediate(() => {
        const imageExport = messages[messages.length - 1];
        assert.match(imageExport.source, /fill: linear\(/);
        assert.match(imageExport.source, /image-source-width: 1920;/);
        assert.match(imageExport.source, /image-source-height: 1080;/);
        assert.match(imageExport.source, /image-scale-mode: fit;/);
        assert.ok(imageExport.diagnostics.some((item) => item.code === "W_IMAGE_PLACEHOLDER"));
        const imageData = providerDataFor(imageExport.source, "98:image");
        assert.strictEqual(imageData.imageFills[0].imageHash, "fixture-image-1920x1080");
        assert.strictEqual(imageData.imageFills[0].scaleMode, "FIT");
        assert.strictEqual(imageData.imageFills[0].sourceWidth, 1920);
        assert.strictEqual(imageData.imageFills[0].sourceHeight, 1080);

        // Large exports stay in the plugin main context during live synchronization. Sending only
        // a bounded preview avoids repeatedly cloning megabyte-scale source strings into the UI.
        const hugeText = base({
          type: "TEXT", id: "98:1", name: "Huge Source",
          width: 400, height: 40,
          absoluteBoundingBox: { x: 0, y: 0, width: 400, height: 40 },
          characters: "x".repeat(210000),
          fontSize: 14,
          fontWeight: 400,
          fontName: { family: "Inter", style: "Regular" },
          textDecoration: "NONE",
          fills: [solid(1, 1, 1)],
        });
        figma.currentPage.selection = [hugeText];
        figma.ui.onmessage({ type: "refresh" });
        const lazyExport = messages[messages.length - 1];
        assert.strictEqual(lazyExport.type, "export-result");
        assert.strictEqual(lazyExport.source, "");
        assert.strictEqual(lazyExport.sourceAvailable, true);
        assert.ok(lazyExport.sourceLength > 210000);
        assert.ok(lazyExport.sourcePreview.length <= 200200);

        figma.ui.onmessage({ type: "request-source", action: "download" });
        const payload = messages[messages.length - 1];
        assert.strictEqual(payload.type, "source-payload");
        assert.strictEqual(payload.action, "download");
        assert.strictEqual(payload.filename, lazyExport.filename);
        assert.ok(payload.source.length > 210000);
        console.log("Figma exporter -> .slugui -> AOT compiler integration passed");
      });
    });
  });
});
