"use strict";
// Regression: linked library paints and mode-dependent variable aliases become concrete SlugUI.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const vm = require("node:vm");
const { spawnSync } = require("node:child_process");

const rootDir = path.resolve(__dirname, "..");
const code = fs.readFileSync(path.join(rootDir, "tools/figma-slugui/code.js"), "utf8");
const messages = [];
const solid = (color, other = {}) => ({
  type: "SOLID", color, visible: true, opacity: 1, ...other,
});
const node = (type, id, fields = {}) => ({
  type, id, name: type + id, visible: true, opacity: 1,
  x: 0, y: 0, width: 90, height: 50, fills: [], strokes: [],
  children: [], layoutMode: "NONE", ...fields,
});
const variables = {
  fill: { resolveForConsumer(n) {
    return { resolvedType: "COLOR",
      value: n.mode === "dark" ? { r: 1, g: 0.25, b: 0 } : { r: 0, g: 0.5, b: 1 } };
  } },
  stroke: { resolveForConsumer() {
    return { resolvedType: "COLOR", value: { r: 0.5, g: 0, b: 1 } };
  } },
  opacity: { resolveForConsumer() {
    return { resolvedType: "FLOAT", value: 0.5 };
  } },
  weight: { resolveForConsumer() {
    return { resolvedType: "FLOAT", value: 3.75 };
  } },
  "style-color": { resolveForConsumer() {
    return { resolvedType: "COLOR", value: { r: 0.2, g: 0.4, b: 0.8 } };
  } },
};
const alias = (id) => ({ type: "VARIABLE_ALIAS", id });
const styled = node("RECTANGLE", "300:2", {
  name: "Linked Style", x: 10, y: 10, fills: [],
  fillStyleId: "style:library", width: 20, height: 30,
});
const variable = node("RECTANGLE", "300:3", {
  name: "Mode Aware", x: 35, y: 10, mode: "light",
  fills: [solid({ r: 0, g: 0, b: 0 })],
  boundVariables: {
    fills: [alias("fill")], opacity: alias("opacity"),
    strokeWeight: alias("weight"),
  },
  strokes: [solid({ r: 0, g: 0, b: 0 },
    { boundVariables: { color: alias("stroke") } })],
  strokeWeight: 1, strokeAlign: "OUTSIDE",
});
const frame = node("FRAME", "300:1", {
  name: "Clipped Frame", clipsContent: true, cornerRadius: 5,
  width: 100, height: 100, children: [styled, variable],
});
styled.parent = frame;
variable.parent = frame;
const figma = {
  mixed: Symbol("mixed"), fileKey: "token-library-fixture",
  root: { documentColorProfile: "SRGB" },
  currentPage: { selection: [frame], on() {}, off() {} },
  ui: { onmessage: null, postMessage(m) { messages.push(m); } },
  on() {}, showUI() {}, closePlugin() {},
  getStyleByIdAsync(id) {
    if (id === "style:missing") return Promise.resolve(null);
    assert.equal(id, "style:library");
    return Promise.resolve({ paints: [
      solid({ r: 0, g: 0, b: 0 }, { boundVariables: { color: alias("style-color") } }),
    ] });
  },
  variables: {
    getVariableByIdAsync(id) { return Promise.resolve(variables[id] || null); },
  },
};
vm.runInNewContext(code, { figma, __html__: "", console, setTimeout, clearTimeout });
async function nextExport(previousCount) {
  for (let i = 0; i < 20; i++) {
    if (messages.length > previousCount) return messages[messages.length - 1];
    await new Promise((resolve) => setImmediate(resolve));
  }
  throw new Error("Async library value resolution did not complete");
}
(async () => {
  const first = await nextExport(0);
  assert.match(first.source, /\/\/ exporter-revision: 2026-11-22-stroke-v6/, "new Figma export identifies its compiled plugin revision");
  assert.match(first.source, /fill: #3366cc;/, "linked library style resolves to a color");
  assert.match(first.source, /fill: #0080ff80;/,
    "color variable uses active mode, node opacity becomes explicit alpha");
  assert.match(first.source, /stroke: #8000ff80;/,
    "stroke variable uses resolved consumer color and node opacity");
  assert.match(first.source, /stroke-width: 3.75;/);
  assert.match(first.source, /stroke-align: outside;/);
  assert.match(first.source, /clip: true;/);
  assert.equal(JSON.parse(JSON.parse(first.source.match(
    /source-provider-data: ("(?:\\.|[^"\\])*");/)[1])).fillStyleId, undefined);
  assert.match(first.source, /\\"fillStyleId\\":\\"style:library\\"/);
  assert.doesNotMatch(first.source, /fill: #000000;/,
    "raw bound paint must not leak in place of the library value");
  const target = path.join(os.tmpdir(), "slugui-library-fixture-" + process.pid + ".slugui");
  try {
    fs.writeFileSync(target, first.source);
    const python = process.platform === "win32" ? "python" : "python3";
    const check = spawnSync(python, [
      path.join(rootDir, "tools/slugui_compiler.py"), target, "--check",
    ], { encoding: "utf8" });
    assert.equal(check.status, 0, check.stderr || check.stdout);
  } finally { fs.rmSync(target, { force: true }); }

  variable.mode = "dark";
  const count = messages.length;
  figma.ui.onmessage({ type: "refresh" });
  const second = await nextExport(count);
  assert.match(second.source, /fill: #ff400080;/,
    "manual refresh must fetch effective variable values in the new mode");
  assert.equal(styled.fills.length, 0, "export must not mutate Figma styles");
  assert.equal(variable.fills[0].color.r, 0, "export must not mutate Figma paints");

  // A missing library must be an explicit fidelity error, not a silently transparent node.
  const missing = node("RECTANGLE", "300:4", {
    fills: [], fillStyleId: "style:missing",
  });
  figma.currentPage.selection = [missing];
  const missingCount = messages.length;
  figma.ui.onmessage({ type: "refresh" });
  const missingExport = await nextExport(missingCount);
  assert.ok(missingExport.diagnostics.some((item) => item.code === "E_STYLE_UNRESOLVED"));
  assert.ok(missingExport.fidelityCounts.unsupported >= 1);

  // The Figma node's effective paints outrank the linked style's default snapshot.
  const effective = node("RECTANGLE", "300:5", {
    fills: [solid({ r: 1, g: 0, b: 0 })], fillStyleId: "style:missing",
  });
  figma.currentPage.selection = [effective];
  const effectiveCount = messages.length;
  figma.ui.onmessage({ type: "refresh" });
  const effectiveExport = await nextExport(effectiveCount);
  assert.match(effectiveExport.source, /fill: #ff0000;/);
  assert.ok(!effectiveExport.diagnostics.some((item) => item.code === "E_STYLE_UNRESOLVED"));
  // Color pickers often use a closed rainbow ramp: red -> rainbow -> red.
  // Reducing it to first/last falsely produces a flat red rectangle.
  const rainbow = node("RECTANGLE", "300:6", {
    fills: [{
      type: "GRADIENT_LINEAR", visible: true, opacity: 1,
      gradientStops: [
        { position: 0, color: { r: 1, g: 0, b: 0, a: 1 } },
        { position: 0.5, color: { r: 0, g: 1, b: 0, a: 1 } },
        { position: 1, color: { r: 1, g: 0, b: 0, a: 1 } },
      ],
    }],
  });
  figma.currentPage.selection = [rainbow];
  const rainbowCount = messages.length;
  figma.ui.onmessage({ type: "refresh" });
  const rainbowExport = await nextExport(rainbowCount);
  assert.match(rainbowExport.source,
    /linear-stops\(\[stop\(0, #ff0000\), stop\(0\.5, #00ff00\), stop\(1, #ff0000\)\], 0, 0, 1, 0\)/);
  assert.equal(rainbowExport.fidelityCounts.unsupported, 0);
  const multiPath = path.join(os.tmpdir(), "slugui-gradient-fixture-" + process.pid + ".slugui");
  try {
    fs.writeFileSync(multiPath, rainbowExport.source);
    const check = spawnSync(process.platform === "win32" ? "python" : "python3", [
      path.join(rootDir, "tools/slugui_compiler.py"), multiPath, "--check",
    ], { encoding: "utf8" });
    assert.equal(check.status, 0, check.stderr || check.stdout);
  } finally { fs.rmSync(multiPath, { force: true }); }
  // Reproduce a Figma HSV square from a white undercoat, a skewed
  // white->red gradient and a vertical transparent->black gradient.
  // The importer must preserve ALL three paints, their order and their
  // exact gradient positions while maintaining the center-aligned stroke.
  const stacked = node("FRAME", "300:7", {
    name: "Layered gradient Frame", width: 100, height: 100,
    clipsContent: true, cornerRadius: 12,
    fills: [
      solid({ r: 1, g: 1, b: 1 }),
      {
        type: "GRADIENT_LINEAR", visible: true, opacity: 1,
        gradientTransform: [[2, 1, -0.5], [0, 1, 0]],
        gradientStops: [
          { position: 0, color: { r: 1, g: 1, b: 1, a: 1 } },
          { position: 1, color: { r: 1, g: 0, b: 0, a: 1 } },
        ],
      },
      {
        type: "GRADIENT_LINEAR", visible: true, opacity: 1,
        gradientTransform: [[0, 1, 0], [1, 0, 0]],
        gradientStops: [
          { position: 0, color: { r: 0, g: 0, b: 0, a: 0 } },
          { position: 0.5, color: { r: 0, g: 0, b: 0, a: 0.5 } },
          { position: 1, color: { r: 0, g: 0, b: 0, a: 1 } },
        ],
      },
    ],
    strokes: [solid({ r: 0.5, g: 0.5, b: 0.5 })],
    strokeWeight: 4, strokeAlign: "CENTER",
  });
  figma.currentPage.selection = [stacked];
  const stackedStart = messages.length;
  figma.ui.onmessage({ type: "refresh" });
  const stackedExport = await nextExport(stackedStart);
  assert.match(stackedExport.source, /fill-layers: \[/,
    "all fills must be preserved instead of discarding the HSV square gradients");
  assert.match(stackedExport.source, /linear\(#ffffff, #ff0000, 0\.2, 0\.1, 0\.6, 0\.3\)/,
    "skewed gradient t=2x+y-0.5 must retain full affine geometry");
  assert.match(stackedExport.source, /linear-stops\(\[stop\(0, #00000000\)/,
    "black alpha ramp must preserve transparent and intermediate stops");
  assert.match(stackedExport.source, /stroke-align: center;/);
  assert.match(stackedExport.source, /clip: true;/);
  assert.ok(!stackedExport.diagnostics.some((x) => x.code === "W_MULTIPLE_FILLS"));
  const layeredPath = path.join(os.tmpdir(), "slugui-layered-" + process.pid + ".slugui");
  try {
    fs.writeFileSync(layeredPath, stackedExport.source);
    const result = spawnSync(process.platform === "win32" ? "python" : "python3", [
      path.join(rootDir, "tools/slugui_compiler.py"), layeredPath, "--check",
    ], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr || result.stdout);
  } finally { fs.rmSync(layeredPath, { force: true }); }

  // Geometry must use the Figma local coordinate frame, not its rotated
  // absoluteBoundingBox. A nested child of a rotated Frame is not supposed
  // to have its page AABB subtracted from its parent's page AABB.
  const rotatedChild = node("RECTANGLE", "300:rotchild", {
    name: "Nested rotated child", x: 99, y: 888, width: 20, height: 30,
    absoluteBoundingBox: {x: 680, y: 900, width: 30, height: 20},
    relativeTransform: [[0, -1, 70], [1, 0, 10]],
    fills: [solid({r:1,g:0,b:0})],
  });
  const nestedChild = node("RECTANGLE", "300:deep", {
    name: "Deep child", width: 15, height: 12,
    absoluteBoundingBox: {x: -200, y: -300, width: 15, height: 12},
    // GROUP is transparent to Figma's relativeTransform. Its translation is
    // relative to the containing FRAME, not to the rotated parent GROUP.
    relativeTransform: [[0, -1, 23], [1, 0, 52]],
    absoluteTransform: [[-1, 0, 348], [0, -1, 223]],
    fills: [solid({r:0,g:1,b:0})],
  });
  const nestedGroup = node("GROUP", "300:group", {
    name: "Rotated group", width: 40, height: 50,
    absoluteBoundingBox: {x: 500, y: 900, width: 50, height: 40},
    relativeTransform: [[0, -1, 30], [1, 0, 40]],
    children: [nestedChild],
  });
  const rotatedFrame = node("FRAME", "300:frame", {
    name: "Rotated Frame", width: 100, height: 80,
    absoluteBoundingBox: {x: 320, y: 200, width: 80, height: 100},
    absoluteTransform: [[0, -1, 400], [1, 0, 200]],
    children: [rotatedChild, nestedGroup],
  });
  rotatedChild.parent = rotatedFrame;
  nestedGroup.parent = rotatedFrame;
  nestedChild.parent = nestedGroup;
  figma.currentPage.selection = [rotatedFrame];
  const rotStart = messages.length;
  figma.ui.onmessage({type:"refresh"});
  const rotResult = await nextExport(rotStart);
  assert.ok(rotResult.source.includes("Absolute Rotated_Frame_300_frame {\n      x: 80px;\n      y: 0px;\n      width: 100px;\n      height: 80px;\n      transform: [0, 1, -1, 0];"));
  assert.ok(rotResult.source.includes("Rectangle Nested_rotated_child_300_rotchild {\n        x: 70px;\n        y: 10px;\n        width: 20px;\n        height: 30px;\n        transform: [0, 1, -1, 0];"));
  assert.ok(rotResult.source.includes("Absolute Rotated_group_300_group {\n        x: 30px;\n        y: 40px;\n        width: 40px;\n        height: 50px;\n        transform: [0, 1, -1, 0];"));
  assert.ok(rotResult.source.includes("Rectangle Deep_child_300_deep {\n          x: 12px;\n          y: 7px;"));
  assert.ok(!rotResult.diagnostics.some((item)=>item.code==="W_TRANSFORM"));
  const transformPath = path.join(os.tmpdir(), "slugui-rotated-" + process.pid + ".slugui");
  try {
    fs.writeFileSync(transformPath, rotResult.source);
    const check = spawnSync(process.platform === "win32" ? "python" : "python3", [
      path.join(rootDir,"tools/slugui_compiler.py"), transformPath, "--check",
    ],{encoding:"utf8"});
    assert.equal(check.status, 0, check.stderr||check.stdout);
  } finally { fs.rmSync(transformPath,{force:true}); }

  // Exact Color Picker regression: the second chevron's Figma
  // relativeTransform has PAGE/FRAME translation despite its direct GROUP
  // parent. The group itself is 428px from the Frame's left edge.
  const flipped = node("RECTANGLE", "1:155", {
    name: "Chevron mirror", width: 5, height: 10,
    relativeTransform: [[-1,0,473],[0,1,18]],
    absoluteBoundingBox: {x:468,y:18,width:5,height:10},
    fills: [solid({r:0.7,g:0.7,b:0.7})],
  });
  const group = node("GROUP", "1:159", {
    name: "Chevron Group", width: 45, height: 10,
    absoluteBoundingBox: {x:428,y:18,width:45,height:10},
    children: [flipped],
  });
  const host = node("FRAME", "1:42", {
    name: "Color Picker", width:787,height:434,
    absoluteTransform:[[1,0,0],[0,1,0]],
    absoluteBoundingBox: {x:0,y:0,width:787,height:434},
    children:[group],
  });
  flipped.parent=group;group.parent=host;
  figma.currentPage.selection=[host];
  const flipStart=messages.length;
  figma.ui.onmessage({type:"refresh"});
  const flipExport=await nextExport(flipStart);
  const flippedBody = flipExport.source.slice(
    flipExport.source.indexOf("Rectangle Chevron_mirror_1_155 {"));
  assert.ok(flippedBody.includes("x: 45px;") &&
            flippedBody.includes("y: 0px;") &&
            flippedBody.includes("transform: [-1, 0, 0, 1];"),
    "mirrored arrow retains its reflected Figma pose and group-local position");

  // The real control uses STROKE-backed VECTOR chevrons, not rectangles.
  // Preserve the outline in node-local coordinates, then reflect via the
  // affine basis without baking a second reflection into path data.
  const arrowPath = "M0 0 L5 5 L0 10 L1 10 L6 5 L1 0 Z";
  const vectorArrow = node("VECTOR", "1:156", {
    name:"Stroke chevron",width:5,height:10,
    relativeTransform:[[-1,0,473],[0,1,18]],
    absoluteBoundingBox:{x:468,y:18,width:5,height:10},
    strokes:[solid({r:0.69,g:0.69,b:0.69})],
    strokeGeometry:[{data:arrowPath,windingRule:"NONZERO"}],
    strokeWeight:2.5,strokeAlign:"CENTER",
  });
  group.children.push(vectorArrow);
  vectorArrow.parent=group;
  const vectorStart=messages.length;
  figma.ui.onmessage({type:"refresh"});
  const vectorExport=await nextExport(vectorStart);
  const chevron = vectorExport.source.slice(
    vectorExport.source.indexOf("Shape Stroke_chevron_1_156 {"));
  assert.ok(chevron.includes("x: 45px;") &&
            chevron.includes("transform: [-1, 0, 0, 1];") &&
            chevron.includes("stroke-path-data: "+JSON.stringify(arrowPath)+";"),
            "stroke VECTOR chevron must not flatten or lose its reflection");
  const arrowFile=path.join(os.tmpdir(),"slugui-chevron-"+process.pid+".slugui");
  try {
    fs.writeFileSync(arrowFile,vectorExport.source);
    const check=spawnSync(process.platform==="win32"?"python":"python3",[
      path.join(rootDir,"tools/slugui_compiler.py"),arrowFile,"--check",
    ],{encoding:"utf8"});
    assert.equal(check.status,0,check.stderr||check.stdout);
  } finally {fs.rmSync(arrowFile,{force:true});}

  // Figma's red->rainbow->red ANGULAR gradient cannot be approximated by
  // linear(red,red). Preserve its complete stops + 2x3 paint matrix.
  const nonlinearKinds=["ANGULAR","RADIAL","DIAMOND"];
  const paints=nonlinearKinds.map((kind)=>node("RECTANGLE","400:"+kind,{
    name:kind,width:64,height:64,
    fills:[{type:"GRADIENT_"+kind,visible:true,opacity:1,
      gradientTransform:[[1,0.3,0.05],[-0.2,1,0.1]],
      gradientStops:[
        {position:0,color:{r:1,g:0,b:0,a:1}},
        {position:0.5,color:{r:0,g:1,b:0,a:0.8}},
        {position:1,color:{r:1,g:0,b:0,a:1}},
      ]}],
  }));
  const gradientFrame=node("FRAME","400:frame",{children:paints,width:200,height:80});
  paints.forEach((n)=>n.parent=gradientFrame);
  figma.currentPage.selection=[gradientFrame];
  const gradStart=messages.length;
  figma.ui.onmessage({type:"refresh"});
  const gradientExport=await nextExport(gradStart);
  for (const kind of ["angular","radial","diamond"]) {
    assert.ok(gradientExport.source.includes(kind+"-stops([stop(0, #ff0000), stop(0.5, #00ff00cc), stop(1, #ff0000)], 1, 0.3, 0.05, -0.2, 1, 0.1)"),
      "Figma "+kind+" retains all stop colors and matrix");
  }
  const gradientPath = path.join(os.tmpdir(), "slugui-figma-stops-"+process.pid+".slugui");
  try {
    fs.writeFileSync(gradientPath, gradientExport.source);
    const check = spawnSync(process.platform === "win32" ? "python" : "python3", [
      path.join(rootDir,"tools/slugui_compiler.py"), gradientPath, "--check",
    ],{encoding:"utf8"});
    assert.equal(check.status,0,check.stderr || check.stdout);
  } finally {fs.rmSync(gradientPath,{force:true});}

  // A flat two-stop Figma gradient must NOT silently look like a successful
  // rainbow export: it is a source-data issue, not a Vulkan interpolation bug.
  const flatPaint = node("RECTANGLE","400:flat",{
    name:"Authoring flat gradient",fills:[{
      type:"GRADIENT_LINEAR",visible:true,opacity:1,
      gradientStops:[
        {position:0,color:{r:1,g:0,b:0,a:1}},
        {position:1,color:{r:1,g:0,b:0,a:1}},
      ],
      gradientTransform:[[1,0,0],[0,1,0]],
    }],
  });
  figma.currentPage.selection=[flatPaint];
  const flatStart=messages.length;
  figma.ui.onmessage({type:"refresh"});
  const flatResult=await nextExport(flatStart);
  assert.equal(flatResult.exporterRevision,"2026-11-22-stroke-v6");
  assert.equal(flatResult.stats.flatGradientCount,1);
  assert.equal(flatResult.stats.gradientPaintCount,1);
  assert.ok(flatResult.diagnostics.some((v)=>v.code==="W_FLAT_GRADIENT"));
  assert.ok(flatResult.source.includes("gradientAudit"),
            "raw Figma stops must remain inspectable in provider metadata");

  // Native Figma PNG reference can be requested without modifying the source
  // document, allowing a later objective pixel comparison against Slugia.
  const pngBytes = [137,80,78,71,13,10,26,10];
  const imageFrame=node("FRAME","500:png",{
    exportAsync:async (options)=>{
      assert.equal(options.format,"PNG");
      return Uint8Array.from(pngBytes);
    }
  });
  figma.currentPage.selection=[imageFrame];
  const pngMessageStart=messages.length;
  figma.ui.onmessage({type:"reference-png"});
  const pngReply=await nextExport(pngMessageStart);
  assert.equal(pngReply.type,"reference-payload");
  assert.deepEqual(Array.from(pngReply.png),pngBytes);
  assert.match(pngReply.filename,/\.figma-reference\.png$/);

  // Real ColorPicker handle: a 25x25 ellipse, mirrored horizontally, with
  // outside stroke 0.938px. outlineStroke() yields a 26.875px local outline.
  // The correct stroke placement is [-0.9375,-0.9375,26.875,26.875] px.
  // The old exporter placed it at [0,0,26.875,26.875], visibly shifting the
  // hairline out of registration with the white cursor circle.
  const handle = node("ELLIPSE","1:44",{
    name:"Handle", width:25,height:25,
    relativeTransform:[[-1,0,400],[0,1,-12]],
    absoluteBoundingBox:{x:375,y:-12,width:25,height:25},
    fillGeometry:[{data:"M0 0H25V25H0Z",windingRule:"NONZERO"}],
    fills:[solid({r:1,g:1,b:1})],
    strokes:[solid({r:0,g:0,b:0},{opacity:0.4})],
    strokeWeight:0.938,strokeAlign:"OUTSIDE",
    outlineStroke() {
      return {
        type:"VECTOR",width:26.875,height:26.875,
        fillGeometry:[{data:"M0 0H26.875V26.875H0Z",windingRule:"NONZERO"}],
        remove(){},
      };
    },
  });
  const handleFrame=node("FRAME","1:43",{
    name:"Color Frame",width:388,height:388,children:[handle],
    absoluteTransform:[[1,0,0],[0,1,0]],
    absoluteBoundingBox:{x:0,y:0,width:388,height:388},
  });
  handle.parent=handleFrame;
  figma.currentPage.selection=[handleFrame];
  const handleStart=messages.length;
  figma.ui.onmessage({type:"refresh"});
  const handleResult=await nextExport(handleStart);
  const handleBody=handleResult.source.slice(
    handleResult.source.indexOf("Shape Handle_1_44 {"));
  assert.ok(handleBody.includes(
    "stroke-placement: [-0.9375px, -0.9375px, 26.875px, 26.875px];"),
    "mirrored ColorPicker handle must center its 0.938px OUTSIDE stroke");
  const handleSource=path.join(os.tmpdir(),"slugui-outside-handle-"+process.pid+".slugui");
  const handleHeader=handleSource+".hpp";
  try {
    fs.writeFileSync(handleSource,handleResult.source);
    const compile=spawnSync(process.platform==="win32"?"python":"python3",[
      path.join(rootDir,"tools/slugui_compiler.py"),handleSource,
      "--output",handleHeader,
    ],{encoding:"utf8"});
    assert.equal(compile.status,0,compile.stderr || compile.stdout);
    const header=fs.readFileSync(handleHeader,"utf8");
    assert.match(header,/strokePlacement = slugia::Rect\{-0\.0375f, -0\.0375f, 1\.075f, 1\.075f\}/,
      "C++ stroke viewport must include the full 0.9375px left/top inset");
  } finally {
    fs.rmSync(handleSource,{force:true});
    fs.rmSync(handleHeader,{force:true});
  }

  console.log("Figma Stroke v6 / 0.938px outside stroke / gradients / PNG reference passed");
})().catch((error) => { console.error(error); process.exitCode = 1; });
