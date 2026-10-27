"use strict";

const FORMAT_VERSION = 3;
const INLINE_SOURCE_LIMIT = 200000;
const diagnostics = [];
const fidelityCounts = { native: 0, approximated: 0, bakedVector: 0, unsupported: 0 };
const vectorGeometryCache = new Map();
const styledTextCache = new Map();
const mainComponentCache = new Map();
const imageInfoCache = new Map();
const indentCache = [""];
let targetedNodeChangeEvents = false;
let exportContext = null;

function createExportContext() {
  return {
    paints: new WeakMap(),
    strokes: new WeakMap(),
    allChildren: new WeakMap(),
    children: new WeakMap(),
    kinds: new WeakMap(),
    bounds: new WeakMap(),
    scenePaths: new WeakMap(),
    parentIndexes: new WeakMap(),
    nodeIds: new Set(),
    dependencyIds: new Set(),
    dependencyRoots: new WeakSet(),
    nodeCount: 0,
    vectorCacheHits: 0,
    textCacheHits: 0,
    temporaryMutation: false,
  };
}

function stableJson(value, depth) {
  depth = depth || 0;
  if (depth > 5 || value === undefined || typeof value === "function") return null;
  if (value === figma.mixed) return "__mixed__";
  if (value === null || typeof value === "string" || typeof value === "boolean" ||
      typeof value === "number") return value;
  if (Array.isArray(value)) return value.map((item) => stableJson(item, depth + 1));
  if (typeof value !== "object") return String(value);
  const result = {};
  for (const key of Object.keys(value).sort()) {
    const resolved = stableJson(value[key], depth + 1);
    if (resolved !== null) result[key] = resolved;
  }
  return result;
}

function metadataJson(value) {
  if (!value || typeof value !== "object") return "";
  try { return JSON.stringify(stableJson(value, 0)); } catch (_) { return ""; }
}

function compactComponentDefinitions(definitions) {
  if (!definitions || typeof definitions !== "object") return definitions;
  const result = {};
  for (const [name, definition] of Object.entries(definitions)) {
    if (!definition || typeof definition !== "object") continue;
    const compact = {};
    if ("type" in definition) compact.type = definition.type;
    if ("defaultValue" in definition) compact.defaultValue = definition.defaultValue;
    if (Array.isArray(definition.variantOptions))
      compact.variantOptions = Array.from(definition.variantOptions);
    if (Array.isArray(definition.preferredValues)) {
      // INSTANCE_SWAP candidate lists can contain hundreds of component keys and are repeated
      // on every instance. They are discovery hints, not the selected instance state.
      compact.preferredValueCount = definition.preferredValues.length;
    }
    result[name] = compact;
  }
  return result;
}

function stablePropertyHash(value) {
  let hash = 2166136261 >>> 0;
  const text = String(value || "");
  for (let index = 0; index < text.length; ++index) {
    hash ^= text.charCodeAt(index);
    hash = Math.imul(hash, 16777619) >>> 0;
  }
  return hash.toString(16).padStart(8, "0");
}

function runtimePropertyIdentifier(owner, name) {
  const readable = String(name || "property").replace(/[^A-Za-z0-9_]+/g, "_")
    .replace(/^([0-9])/, "_$1").slice(0, 40) || "property";
  return "figma_" + readable + "_" +
    stablePropertyHash(String(owner && owner.id || "") + "\u0000" + String(name || ""));
}

function componentDefinitionsFor(node) {
  if (!node || typeof node !== "object") return null;
  const main = instanceMainComponent(node);
  const set = node.parent && node.parent.type === "COMPONENT_SET"
    ? node.parent
    : (main && main.parent && main.parent.type === "COMPONENT_SET" ? main.parent : null);
  return safeNodeValue(node, "componentPropertyDefinitions", null) ||
    safeNodeValue(main, "componentPropertyDefinitions", null) ||
    safeNodeValue(set, "componentPropertyDefinitions", null);
}

function runtimePropertyScope(node) {
  if (!node || typeof node !== "object") return null;
  if (node.type === "INSTANCE" || node.type === "COMPONENT_SET") return node;
  if (node.type === "COMPONENT" && node.parent && node.parent.type === "COMPONENT_SET") {
    const setDefinitions = safeNodeValue(node.parent, "componentPropertyDefinitions", null);
    if (setDefinitions && typeof setDefinitions === "object") return node.parent;
  }
  return node;
}

function runtimePropertyOwner(node, name) {
  let current = node;
  let depth = 0;
  while (current && typeof current === "object" && depth++ < 256) {
    const definitions = componentDefinitionsFor(current);
    if (definitions && typeof definitions === "object" && name in definitions)
      return runtimePropertyScope(current);
    current = current.parent;
  }
  return null;
}

function runtimePropertyReference(node, field, expectedType) {
  const references = safeNodeValue(node, "componentPropertyReferences", null);
  if (!references || typeof references !== "object") return "";
  const name = references[field];
  if (typeof name !== "string" || !name) return "";
  const owner = runtimePropertyOwner(node, name);
  if (!owner) return "";
  const definitions = componentDefinitionsFor(owner);
  const definition = definitions && definitions[name];
  if (!definition || String(definition.type || "").toUpperCase() !== expectedType) return "";
  return runtimePropertyIdentifier(owner, name);
}

function runtimeComponentProperties(selection) {
  const result = [];
  const seen = new Set();
  const stack = [];
  for (const root of topLevelSelection(Array.from(selection || []))) stack.push(root);
  while (stack.length > 0) {
    const node = stack.pop();
    if (!node || typeof node !== "object") continue;
    if (node.type === "COMPONENT_SET" || node.type === "COMPONENT" ||
        node.type === "INSTANCE") {
      const definitions = componentDefinitionsFor(node);
      if (definitions && typeof definitions === "object") {
        const current = safeNodeValue(node, "componentProperties", null);
        const variants = safeNodeValue(node, "variantProperties", null);
        const scope = runtimePropertyScope(node);
        for (const [name, definition] of Object.entries(definitions)) {
          if (!definition || typeof definition !== "object") continue;
          const type = String(definition.type || "").toUpperCase();
          if (!["BOOLEAN", "TEXT", "VARIANT"].includes(type)) continue;
          const identifier = runtimePropertyIdentifier(scope, name);
          if (seen.has(identifier)) continue;
          let value = definition.defaultValue;
          if (current && current[name] && "value" in current[name]) value = current[name].value;
          else if (type === "VARIANT" && variants && name in variants) value = variants[name];
          if (type === "BOOLEAN" && typeof value !== "boolean") continue;
          if ((type === "TEXT" || type === "VARIANT") && typeof value !== "string") continue;
          seen.add(identifier);
          result.push({ identifier, type, value });
        }
      }
    }
    const children = allChildrenOf(node);
    for (let index = children.length - 1; index >= 0; --index) stack.push(children[index]);
  }
  return result;
}

function diagnostic(node, code, message, fidelity) {
  diagnostics.push({
    code,
    nodeId: node && node.id ? node.id : "",
    nodeName: node && node.name ? node.name : "",
    message,
    fidelity: fidelity || "Approximated",
  });
}

function isMixed(value) {
  return value === figma.mixed;
}

function finite(value, fallback) {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function hasNonIdentityAxes(transform) {
  if (!Array.isArray(transform) || transform.length < 2 ||
      !Array.isArray(transform[0]) || !Array.isArray(transform[1])) return false;
  const m00 = finite(transform[0][0], 1);
  const m01 = finite(transform[0][1], 0);
  const m10 = finite(transform[1][0], 0);
  const m11 = finite(transform[1][1], 1);
  return Math.abs(m00 - 1) > 0.0001 || Math.abs(m01) > 0.0001 ||
    Math.abs(m10) > 0.0001 || Math.abs(m11 - 1) > 0.0001;
}

function number(value) {
  const rounded = Math.round(finite(value, 0) * 1000) / 1000;
  return String(Object.is(rounded, -0) ? 0 : rounded);
}

function slugString(value) {
  return JSON.stringify(String(value));
}

function identifier(value, fallback) {
  let result = String(value || fallback || "node")
    .normalize("NFKD")
    .replace(/[^A-Za-z0-9_]+/g, "_")
    .replace(/^_+|_+$/g, "");
  if (!result) result = fallback || "node";
  if (/^[0-9]/.test(result)) result = "_" + result;
  return result;
}

function elementName(node) {
  const id = String(node.id || "0").replace(/[^A-Za-z0-9]+/g, "_");
  return identifier(node.name, "node") + "_" + id;
}

function componentName(selection) {
  if (selection.length === 1) {
    const base = identifier(selection[0].name, "FigmaSelection");
    return base.charAt(0).toUpperCase() + base.slice(1) + "Generated";
  }
  return "FigmaSelectionGenerated";
}

function hexByte(value) {
  return Math.max(0, Math.min(255, Math.round(finite(value, 0) * 255)))
    .toString(16).padStart(2, "0");
}

function srgbDecode(value) {
  value = Math.max(0, finite(value, 0));
  return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4);
}

function srgbEncode(value) {
  value = Math.max(0, finite(value, 0));
  return value <= 0.0031308 ? value * 12.92 : 1.055 * Math.pow(value, 1 / 2.4) - 0.055;
}

function colorInSrgb(color) {
  const profile = figma.root && figma.root.documentColorProfile;
  if (profile !== "DISPLAY_P3") return color;
  const p3r = srgbDecode(color.r);
  const p3g = srgbDecode(color.g);
  const p3b = srgbDecode(color.b);
  return {
    r: srgbEncode(1.224940176 * p3r - 0.224940176 * p3g),
    g: srgbEncode(-0.042056955 * p3r + 1.042056955 * p3g),
    b: srgbEncode(-0.019637555 * p3r - 0.078636046 * p3g + 1.098273601 * p3b),
    a: color.a,
  };
}

function colorHex(color, opacity) {
  color = colorInSrgb(color);
  const alpha = finite(color.a, 1) * finite(opacity, 1);
  const rgb = "#" + hexByte(color.r) + hexByte(color.g) + hexByte(color.b);
  return alpha >= 0.9995 ? rgb : rgb + hexByte(alpha);
}

function visiblePaints(node) {
  if (exportContext && exportContext.paints.has(node)) return exportContext.paints.get(node);
  let result = [];
  if (!("fills" in node) || isMixed(node.fills) || !Array.isArray(node.fills)) {
    if ("fills" in node && isMixed(node.fills)) {
      diagnostic(node, "W_MIXED_FILL", "Mixed fills were replaced with transparent paint.");
    }
  } else {
    result = node.fills.filter((paint) => paint && paint.visible !== false);
  }
  if (exportContext) exportContext.paints.set(node, result);
  return result;
}

function imagePlaceholderPaint(paint, opacity) {
  const key = String((paint && paint.imageHash) || "image");
  let hash = 2166136261 >>> 0;
  for (let index = 0; index < key.length; ++index) {
    hash ^= key.charCodeAt(index);
    hash = Math.imul(hash, 16777619) >>> 0;
  }
  const channel = (shift) => 0.28 + (((hash >>> shift) & 0x3f) / 255);
  const first = colorHex({ r: channel(0), g: channel(6), b: channel(12), a: 1 }, opacity);
  const second = colorHex({ r: channel(18), g: channel(3), b: channel(9), a: 1 }, opacity);
  return "linear(" + first + ", " + second + ")";
}

function visibleStrokes(node) {
  if (exportContext && exportContext.strokes.has(node)) return exportContext.strokes.get(node);
  let result = [];
  if (!("strokes" in node) || isMixed(node.strokes) || !Array.isArray(node.strokes)) {
    if ("strokes" in node && isMixed(node.strokes)) {
      diagnostic(node, "W_MIXED_STROKE", "Mixed strokes were replaced with transparent paint.");
    }
  } else {
    result = node.strokes.filter((paint) => paint && paint.visible !== false);
  }
  if (exportContext) exportContext.strokes.set(node, result);
  return result;
}

function paintListSource(node, paints, role) {
  if (paints.length === 0) return "#00000000";
  if (paints.length > 1) {
    diagnostic(node, role === "fill" ? "W_MULTIPLE_FILLS" : "W_MULTIPLE_STROKES",
      "Only the first visible " + role + " is exported.");
  }
  const paint = paints[0];
  const opacity = finite(paint.opacity, 1) * finite(node.opacity, 1);
  if (paint.boundVariables && Object.keys(paint.boundVariables).length > 0) {
    diagnostic(node, "W_VARIABLE_BAKED", "Bound paint variables are exported as resolved values.");
  }
  if (paint.type === "SOLID") return colorHex(paint.color, opacity);
  if (paint.type === "IMAGE") {
    diagnostic(node, "W_IMAGE_PLACEHOLDER",
      "Image fill is represented by a geometry-preserving placeholder; source resolution and transform are retained in metadata.",
      "Approximated");
    return imagePlaceholderPaint(paint, opacity);
  }
  if (paint.type && paint.type.indexOf("GRADIENT_") === 0) {
    const stops = Array.isArray(paint.gradientStops) ? paint.gradientStops : [];
    if (stops.length < 2) {
      diagnostic(node, "W_EMPTY_GRADIENT", "Gradient without two stops became transparent.");
      return "#00000000";
    }
    if (stops.length > 2) {
      diagnostic(node, "W_GRADIENT_STOPS", "SlugUI currently keeps the first and last gradient stops.");
    }
    if (paint.gradientTransform) {
      diagnostic(node, "W_GRADIENT_TRANSFORM", "Gradient transform is approximated by normalized endpoints.");
    }
    const first = colorHex(stops[0].color, opacity);
    const last = colorHex(stops[stops.length - 1].color, opacity);
    const names = {
      GRADIENT_LINEAR: "linear",
      GRADIENT_RADIAL: "radial",
      GRADIENT_DIAMOND: "diamond",
      GRADIENT_ANGULAR: "linear",
    };
    if (paint.type === "GRADIENT_ANGULAR") {
      diagnostic(node, "W_ANGULAR_GRADIENT", "Angular gradient is approximated as linear.");
    }
    return (names[paint.type] || "linear") + "(" + first + ", " + last + ")";
  }
  diagnostic(node, "E_UNSUPPORTED_PAINT",
    "Video, pattern, and Figma shader " + role + " paints require asset baking.",
    "Unsupported");
  return "#00000000";
}

function paintSource(node) {
  return paintListSource(node, visiblePaints(node), "fill");
}

function strokePaintSource(node) {
  return paintListSource(node, visibleStrokes(node), "stroke");
}

function hasVisualFill(node) {
  return visiblePaints(node).length > 0;
}

function hasVisualStroke(node) {
  return "strokes" in node && !isMixed(node.strokes) && Array.isArray(node.strokes) &&
    node.strokes.some((paint) => paint && paint.visible !== false);
}

function geometryPaths(node, property, fallbackProperty) {
  let paths = property in node && Array.isArray(node[property]) ? node[property] : [];
  if (paths.length === 0 && fallbackProperty &&
      fallbackProperty in node && Array.isArray(node[fallbackProperty])) {
    paths = node[fallbackProperty];
  }
  return paths.filter((path) => path && typeof path.data === "string" && path.data.trim() !== "");
}

function fillGeometryPaths(node) {
  return geometryPaths(node, "fillGeometry", "vectorPaths");
}

function nodeBounds(node, rendered) {
  const source = rendered && node.absoluteRenderBounds ? node.absoluteRenderBounds :
    (node.absoluteBoundingBox || null);
  if (source && [source.x, source.y, source.width, source.height].every(Number.isFinite)) {
    return { x: source.x, y: source.y, width: source.width, height: source.height };
  }
  return {
    x: finite(node.x, 0), y: finite(node.y, 0),
    width: finite(node.width, 0), height: finite(node.height, 0),
  };
}

function exportBounds(node) {
  if (exportContext && exportContext.bounds.has(node)) return exportContext.bounds.get(node);
  const bounds = nodeBounds(node, false);
  let result = bounds;
  if (nodeKind(node) === "Shape" && (bounds.width <= 0 || bounds.height <= 0)) {
    const rendered = nodeBounds(node, true);
    if (rendered.width > 0 && rendered.height > 0) result = rendered;
  }
  if (exportContext) exportContext.bounds.set(node, result);
  return result;
}

function removeTemporary(node) {
  if (!node || typeof node.remove !== "function") return;
  try { node.remove(); } catch (_) { /* the operation may already have consumed it */ }
}

function absoluteBounds(node, rendered) {
  const source = rendered && node.absoluteRenderBounds ? node.absoluteRenderBounds :
    (node.absoluteBoundingBox || null);
  if (source && [source.x, source.y, source.width, source.height].every(Number.isFinite)) {
    return { x: source.x, y: source.y, width: source.width, height: source.height };
  }
  return null;
}

function pathSignature(paths) {
  if (!Array.isArray(paths) || paths.length === 0) return "";
  return paths.map((path) =>
    path && typeof path.data === "string"
      ? String(path.windingRule || "") + ":" + path.data
      : "").join("|");
}

function vectorGeometrySignature(node) {
  return [
    finite(node.width, 0), finite(node.height, 0),
    typeof node.strokeWeight === "number" ? node.strokeWeight : "mixed",
    finite(node.strokeTopWeight, 0), finite(node.strokeRightWeight, 0),
    finite(node.strokeBottomWeight, 0), finite(node.strokeLeftWeight, 0),
    String(node.strokeAlign || ""),
    pathSignature(node.fillGeometry), pathSignature(node.vectorPaths),
    pathSignature(node.strokeGeometry),
    hasVisualStroke(node) ? 1 : 0,
  ].join("~");
}

function bakedVectorGeometry(node) {
  const cacheKey = String(node.id || "");
  const cached = cacheKey ? vectorGeometryCache.get(cacheKey) : null;
  // PageNode.nodechange gives us precise invalidation on current Figma runtimes. Avoid hashing
  // potentially huge SVG path strings on every refresh when the node has not changed.
  const signature = cacheKey && !targetedNodeChangeEvents ? vectorGeometrySignature(node) : "";
  if (cached && (targetedNodeChangeEvents || cached.signature === signature)) {
    if (exportContext) exportContext.vectorCacheHits += 1;
    // Geometry is local to the node, but placement is not. A parent Group/Component can move
    // without changing the child's path data, so never cache absolute bounds with the path.
    return {
      fillPaths: cached.result.fillPaths,
      fillBounds: cached.result.fillPaths.length > 0 ? nodeBounds(node, false) : null,
      strokePaths: cached.result.strokePaths,
      strokeBounds: cached.result.strokePaths.length > 0 ? absoluteBounds(node, true) : null,
    };
  }

  const result = { fillPaths: [], fillBounds: null, strokePaths: [], strokeBounds: null };
  let cacheable = true;
  if (hasVisualFill(node)) {
    try {
      // Figma guarantees fillGeometry is relative to the node. Reading it directly preserves the
      // local coordinate system; flattening a nested clone into the page changes its parent.
      result.fillPaths = fillGeometryPaths(node);
      result.fillBounds = nodeBounds(node, false);
    } catch (error) {
      cacheable = false;
      diagnostic(node, "E_VECTOR_FLATTEN",
        "Figma could not read this vector fill: " + (error && error.message ? error.message : error),
        "Unsupported");
    }
  }
  if (hasVisualStroke(node)) {
    let outlined = null;
    try {
      // Figma exposes strokeGeometry in node-local coordinates, and documents that it always
      // represents the centered stroke. CENTER therefore needs no temporary document mutation.
      // INSIDE/OUTSIDE still use outlineStroke() to resolve alignment exactly.
      if (String(node.strokeAlign || "CENTER") === "CENTER") {
        const centered = geometryPaths(node, "strokeGeometry", null);
        if (centered.length > 0) {
          result.strokePaths = centered;
          result.strokeBounds = absoluteBounds(node, true);
        }
      }
      if (result.strokePaths.length === 0) {
        if (typeof node.outlineStroke !== "function") {
          throw new Error("outlineStroke is unavailable");
        }
        if (exportContext) exportContext.temporaryMutation = true;
        outlined = node.outlineStroke();
        if (!outlined) throw new Error("outlineStroke returned no geometry");
        result.strokePaths = fillGeometryPaths(outlined);
        result.strokeBounds = absoluteBounds(node, true);
        if (result.strokePaths.length === 0) throw new Error("outlined stroke has no fillGeometry");
      }
    } catch (error) {
      cacheable = false;
      result.strokePaths = [];
      result.strokeBounds = null;
      diagnostic(node, "E_VECTOR_STROKE_OUTLINE",
        "Figma could not create exact stroke geometry: " +
          (error && error.message ? error.message : error), "Unsupported");
    } finally {
      removeTemporary(outlined);
    }
  }
  if (cacheable && cacheKey) vectorGeometryCache.set(cacheKey, { signature, result });
  return result;
}

function placementSource(bounds, ownerBounds) {
  if (!bounds || !ownerBounds || bounds.width <= 0 || bounds.height <= 0) return null;
  return "[" + [
    bounds.x - ownerBounds.x, bounds.y - ownerBounds.y, bounds.width, bounds.height,
  ].map((value) => number(value) + "px").join(", ") + "]";
}

function pathDataValue(paths) {
  const values = paths.map((path) => slugString(path.data));
  return values.length === 1 ? values[0] : "[" + values.join(", ") + "]";
}

function pathWindingValue(paths) {
  const values = paths.map((path) =>
    String(path.windingRule || "NONZERO").toUpperCase() === "EVENODD" ? "evenodd" : "nonzero");
  return values.length === 1 ? values[0] : "[" + values.join(", ") + "]";
}

function allChildrenOf(node) {
  if (exportContext && exportContext.allChildren.has(node)) {
    return exportContext.allChildren.get(node);
  }
  let result = [];
  try {
    result = node && "children" in node && Array.isArray(node.children)
      ? node.children : [];
  } catch (_) {}
  if (exportContext) exportContext.allChildren.set(node, result);
  return result;
}

function childrenOf(node) {
  if (exportContext && exportContext.children.has(node)) return exportContext.children.get(node);
  const allChildren = allChildrenOf(node);
  if (exportContext) {
    // Invisible children are not emitted, but their visibility can change while the parent
    // component/group stays selected. Watch their IDs so that transition is synchronized.
    for (const child of allChildren) {
      if (child && child.id) exportContext.nodeIds.add(String(child.id));
    }
  }
  const result = allChildren.filter((child) =>
    child.visible !== false || Boolean(runtimePropertyReference(child, "visible", "BOOLEAN")));
  if (exportContext) exportContext.children.set(node, result);
  return result;
}

function childIndex(parent, child) {
  if (!parent) return -1;
  const children = allChildrenOf(parent);
  if (!exportContext) return children.indexOf(child);
  let indexes = exportContext.parentIndexes.get(parent);
  if (!indexes) {
    indexes = new Map();
    for (let index = 0; index < children.length; ++index) {
      indexes.set(children[index], index);
    }
    exportContext.parentIndexes.set(parent, indexes);
  }
  return indexes.has(child) ? indexes.get(child) : -1;
}

function sceneOrderPath(node) {
  if (exportContext && exportContext.scenePaths.has(node)) {
    return exportContext.scenePaths.get(node);
  }
  const result = [];
  let current = node;
  while (current && current.parent) {
    const index = childIndex(current.parent, current);
    if (index < 0) return [];
    result.unshift(index);
    current = current.parent;
  }
  if (exportContext) exportContext.scenePaths.set(node, result);
  return result;
}

function compareSceneOrder(first, second) {
  const a = sceneOrderPath(first);
  const b = sceneOrderPath(second);
  const shared = Math.min(a.length, b.length);
  for (let index = 0; index < shared; ++index) {
    if (a[index] !== b[index]) return a[index] - b[index];
  }
  return a.length - b.length;
}

function topLevelSelection(selection) {
  const selected = new Set(selection);
  return selection.filter((node) => {
    let parent = node.parent;
    while (parent) {
      if (selected.has(parent)) return false;
      parent = parent.parent;
    }
    return true;
  });
}

function selectionKey(selection) {
  return selection.map((node) => String(node.id || "")).sort().join("|");
}

function containerLayout(node) {
  if (!("layoutMode" in node)) return "absolute";
  if (node.layoutMode === "GRID") return "absolute";
  if (node.layoutMode === "HORIZONTAL" || node.layoutMode === "VERTICAL") {
    // Until SlugUI has font-metric baseline alignment and stroke-aware content boxes,
    // preserve the exact Figma-resolved snapshot instead of silently mapping these to
    // a different semantic layout.
    if (String(safeNodeValue(node, "counterAxisAlignItems", "")) === "BASELINE")
      return "absolute";
    if (safeNodeValue(node, "strokesIncludedInLayout", false) === true)
      return "absolute";
    return node.layoutMode === "HORIZONTAL" ? "row" : "column";
  }
  return "absolute";
}

function nodeKind(node) {
  if (exportContext && exportContext.kinds.has(node)) return exportContext.kinds.get(node);
  let kind = "Absolute";
  if (node.type === "TEXT") {
    kind = "Text";
  } else if (node.type === "RECTANGLE") {
    kind = "Rectangle";
  } else if (["FRAME", "COMPONENT", "INSTANCE", "COMPONENT_SET", "GROUP", "SECTION", "SLOT", "TRANSFORM_GROUP"].includes(node.type)) {
    if (hasVisualFill(node) || hasVisualStroke(node) || "cornerRadius" in node) {
      kind = "Rectangle";
    } else {
      const layout = containerLayout(node);
      kind = layout === "row" ? "Row" : layout === "column" ? "Column" : "Absolute";
    }
  } else if (["VECTOR", "BOOLEAN_OPERATION", "ELLIPSE", "POLYGON", "STAR", "LINE"].includes(node.type)) {
    kind = "Shape";
  }
  if (exportContext) exportContext.kinds.set(node, kind);
  return kind;
}

function textListType(value) {
  if (!value || isMixed(value) || typeof value !== "object") return "";
  const type = String(value.type || "").toUpperCase();
  return ["ORDERED", "UNORDERED", "NONE"].includes(type) ? type : "";
}

function resolvedTextListType(node, segments) {
  const direct = textListType(node.listOptions);
  if (direct) return direct;
  const types = new Set((segments || []).map((segment) =>
    textListType(segment.listOptions)).filter(Boolean));
  return types.size === 1 ? Array.from(types)[0] : "";
}

function resolvedParagraphIndent(node, segments) {
  if (typeof node.paragraphIndent === "number") return node.paragraphIndent;
  const values = new Set((segments || [])
    .filter((segment) => typeof segment.paragraphIndent === "number")
    .map((segment) => segment.paragraphIndent));
  return values.size === 1 ? Array.from(values)[0] : null;
}

function styledTextNeedsApproximation(node, segments) {
  const listType = resolvedTextListType(node, segments);
  const paragraphIndent = resolvedParagraphIndent(node, segments);
  return (segments || []).some((segment) => {
    const segmentList = textListType(segment.listOptions);
    const openType = segment.openTypeFeatures;
    return finite(segment.paragraphSpacing, 0) !== 0 ||
      finite(segment.listSpacing, 0) !== 0 ||
      finite(segment.indentation, 0) !== 0 ||
      (segmentList && listType && segmentList !== listType) ||
      (typeof segment.paragraphIndent === "number" && paragraphIndent !== null &&
       segment.paragraphIndent !== paragraphIndent) ||
      (segment.textDecorationStyle &&
       !["SOLID", "NONE"].includes(String(segment.textDecorationStyle))) ||
      (openType && typeof openType === "object" &&
       Object.values(openType).some((value) => Boolean(value)));
  });
}

function fidelityFor(node, vectorGeometry, textSegments) {
  if (["VECTOR", "BOOLEAN_OPERATION", "ELLIPSE", "POLYGON", "STAR", "LINE"].includes(node.type)) {
    if ((hasVisualFill(node) && (!vectorGeometry || vectorGeometry.fillPaths.length === 0)) ||
        (hasVisualStroke(node) && (!vectorGeometry || vectorGeometry.strokePaths.length === 0))) {
      return "unsupported";
    }
    const paths = vectorGeometry.fillPaths.concat(vectorGeometry.strokePaths);
    return paths.some((path) => path.windingRule === "EVENODD")
      ? "approximated" : "baked-vector";
  }
  const styledText = node.type === "TEXT" && textSegments && textSegments.length > 0;
  if (styledText && styledTextNeedsApproximation(node, textSegments)) return "approximated";
  if (!styledText && "fills" in node && isMixed(node.fills)) return "approximated";
  const paints = styledText ? [] : visiblePaints(node);
  if (paints.length > 0) {
    const paint = paints[0];
    if (paint.type === "IMAGE") return "approximated";
    if (["VIDEO", "PATTERN", "SHADER"].includes(paint.type)) return "unsupported";
    if (paint.boundVariables && Object.keys(paint.boundVariables).length > 0) return "approximated";
    if (paint.type === "GRADIENT_ANGULAR" || paint.gradientTransform ||
        (Array.isArray(paint.gradientStops) && paint.gradientStops.length !== 2)) {
      return "approximated";
    }
  }
  if (node.effects && !isMixed(node.effects) && node.effects.some((effect) => effect.visible !== false)) {
    return "approximated";
  }
  if (!styledText && node.type === "TEXT" &&
      (isMixed(node.fontName) || isMixed(node.fontSize) || isMixed(node.textDecoration))) {
    return "approximated";
  }
  if (finite(node.opacity, 1) < 0.9995 && childrenOf(node).length > 0) {
    return "approximated";
  }
  if (!styledText && visiblePaints(node).length > 1) return "approximated";
  const rotation = safeNodeValue(node, "rotation", 0);
  if ((typeof rotation === "number" && Number.isFinite(rotation) && Math.abs(rotation) > 0.0001) ||
      hasNonIdentityAxes(safeNodeValue(node, "relativeTransform", null))) {
    return "approximated";
  }
  const transformModifiers = safeNodeValue(node, "transformModifiers", null);
  if (Array.isArray(transformModifiers) && transformModifiers.length > 0) return "approximated";
  return "native";
}

function push(lines, depth, text) {
  while (indentCache.length <= depth) {
    indentCache.push(indentCache[indentCache.length - 1] + "  ");
  }
  lines.push(indentCache[depth] + text);
}

function attribute(lines, depth, name, value) {
  push(lines, depth, name + ": " + value + ";");
}

function safeNodeValue(node, property, fallback) {
  try {
    const value = node && property in node ? node[property] : undefined;
    return value === undefined || value === null ? fallback : value;
  } catch (_) {
    return fallback;
  }
}

function instanceMainComponent(node) {
  if (!node || node.type !== "INSTANCE") return null;
  const cacheKey = String(node.id || "");
  if (cacheKey && mainComponentCache.has(cacheKey)) return mainComponentCache.get(cacheKey);
  const direct = safeNodeValue(node, "mainComponent", null);
  if (direct && typeof direct === "object") {
    if (cacheKey) mainComponentCache.set(cacheKey, direct);
    return direct;
  }
  return null;
}

function instanceMainComponentId(node) {
  const direct = instanceMainComponent(node);
  if (direct && direct.id) return String(direct.id);
  return String(safeNodeValue(node, "mainComponentId", "") || "");
}

function selectedInstances(selection) {
  const result = [];
  const seen = new Set();
  const add = (node) => {
    if (!node || node.type !== "INSTANCE") return;
    const key = String(node.id || "");
    if (key && seen.has(key)) return;
    if (key) seen.add(key);
    result.push(node);
  };

  for (const root of topLevelSelection(Array.from(selection || []))) {
    add(root);
    if (!root) continue;
    // Instances can themselves contain nested instances. Dynamic-page Figma may throw from the
    // synchronous mainComponent getter for those descendants too, so prime the complete selected
    // authoring subtree instead of stopping at the first InstanceNode.
    if (typeof root.findAllWithCriteria === "function") {
      try {
        for (const node of root.findAllWithCriteria({ types: ["INSTANCE"] })) add(node);
        continue;
      } catch (_) { /* older/mock runtimes fall back to a local subtree walk */ }
    }
    const stack = Array.from(allChildrenOf(root));
    while (stack.length > 0) {
      const current = stack.pop();
      add(current);
      for (const child of allChildrenOf(current)) stack.push(child);
    }
  }
  return result;
}

function primeImageInfo(selection) {
  if (typeof figma.getImageByHash !== "function") return null;
  const pending = [];
  const seen = new Set();
  const inspect = (node) => {
    if (!node || typeof node !== "object") return;
    const fills = ("fills" in node && !isMixed(node.fills) && Array.isArray(node.fills))
      ? node.fills : [];
    for (const paint of fills) {
      if (!paint || paint.visible === false || paint.type !== "IMAGE" || !paint.imageHash) continue;
      const hash = String(paint.imageHash);
      if (seen.has(hash) || imageInfoCache.has(hash)) continue;
      seen.add(hash);
      let image = null;
      try { image = figma.getImageByHash(hash); } catch (_) {}
      if (!image) {
        imageInfoCache.set(hash, null);
        continue;
      }
      if (typeof image.getSizeAsync === "function") {
        pending.push(Promise.resolve(image.getSizeAsync()).then(
          (size) => imageInfoCache.set(hash, {
            width: finite(size && size.width, 0),
            height: finite(size && size.height, 0),
          }),
          () => imageInfoCache.set(hash, null)));
      } else {
        imageInfoCache.set(hash, null);
      }
    }
  };

  for (const root of topLevelSelection(Array.from(selection || []))) {
    inspect(root);
    if (root && typeof root.findAll === "function") {
      try {
        for (const node of root.findAll()) inspect(node);
        continue;
      } catch (_) {}
    }
    const stack = Array.from(allChildrenOf(root));
    while (stack.length > 0) {
      const current = stack.pop();
      inspect(current);
      for (const child of allChildrenOf(current)) stack.push(child);
    }
  }
  return pending.length > 0 ? Promise.all(pending) : null;
}

function primeMainComponents(selection) {
  const pending = [];
  for (const instance of selectedInstances(selection)) {
    const key = String(instance.id || "");
    if (!key || mainComponentCache.has(key)) continue;
    if (typeof instance.getMainComponentAsync === "function") {
      pending.push(Promise.resolve(instance.getMainComponentAsync()).then(
        (component) => mainComponentCache.set(key, component || null),
        () => mainComponentCache.set(key, null)));
    } else {
      mainComponentCache.set(key, instanceMainComponent(instance));
    }
  }
  return pending.length > 0 ? Promise.all(pending) : null;
}

function rememberTreeIds(root, target) {
  if (!root || typeof root !== "object" || !target) return;
  if (root.id) target.add(String(root.id));

  // Figma's native subtree search avoids repeatedly materializing a fresh children array at every
  // level. We need every descendant id for precise invalidation, so findAll() is the fastest exact
  // traversal when the host exposes it; mocks/older hosts keep the bounded DFS fallback.
  if (typeof root.findAll === "function") {
    try {
      const descendants = root.findAll();
      const limit = Math.min(descendants.length, 8191);
      for (let index = 0; index < limit; ++index) {
        const current = descendants[index];
        if (current && current.id) target.add(String(current.id));
      }
      return;
    } catch (_) {}
  }

  const stack = Array.from(allChildrenOf(root));
  let visited = 1;
  while (stack.length > 0 && visited < 8192) {
    const current = stack.pop();
    if (!current || typeof current !== "object") continue;
    ++visited;
    if (current.id) target.add(String(current.id));
    const children = allChildrenOf(current);
    for (let index = children.length - 1; index >= 0; --index) stack.push(children[index]);
  }
}

function rememberDependencyTree(root) {
  if (!exportContext || !root || typeof root !== "object") return;
  // Large component sets are often referenced by many instances in the same selection. Walking
  // the same definition tree for every instance made exporter cost scale as instances × variants.
  // One walk per dependency root preserves invalidation coverage while keeping it linear.
  if (exportContext.dependencyRoots.has(root)) return;
  exportContext.dependencyRoots.add(root);
  rememberTreeIds(root, exportContext.dependencyIds);
}

function emitSourceMetadata(node, lines, depth, fidelity) {
  attribute(lines, depth, "source-provider", slugString("figma"));
  attribute(lines, depth, "source-document", slugString(figma.fileKey || "local-document"));
  attribute(lines, depth, "source-node", slugString(node.id || ""));
  attribute(lines, depth, "source-name", slugString(node.name || ""));

  const providerData = { nodeType: String(node.type || "") };
  if (node.parent && node.parent.id) providerData.parentNodeId = String(node.parent.id);
  const constraints = safeNodeValue(node, "constraints", null);
  if (constraints && typeof constraints === "object") providerData.constraints = constraints;
  const targetAspectRatio = safeNodeValue(node, "targetAspectRatio", null);
  if (targetAspectRatio && typeof targetAspectRatio === "object")
    providerData.targetAspectRatio = targetAspectRatio;
  if (safeNodeValue(node, "strokesIncludedInLayout", false) === true)
    providerData.strokesIncludedInLayout = true;
  const relativeTransform = safeNodeValue(node, "relativeTransform", null);
  if (hasNonIdentityAxes(relativeTransform)) providerData.relativeTransform = relativeTransform;
  const rotation = safeNodeValue(node, "rotation", 0);
  if (typeof rotation === "number" && Number.isFinite(rotation) && Math.abs(rotation) > 0.0001) {
    providerData.rotation = rotation;
  }
  const transformModifiers = safeNodeValue(node, "transformModifiers", null);
  if (Array.isArray(transformModifiers) && transformModifiers.length > 0) {
    providerData.transformModifiers = transformModifiers;
  }

  // Metadata inspection must not emit fill diagnostics (notably mixed rich-text fills).
  const rawFills = ("fills" in node && !isMixed(node.fills) && Array.isArray(node.fills))
    ? node.fills : [];
  const imageFills = rawFills.filter(
    (paint) => paint && paint.visible !== false && paint.type === "IMAGE");
  if (imageFills.length > 0) {
    providerData.imageFills = imageFills.map((paint) => {
      const hash = String(paint.imageHash || "");
      const info = hash ? imageInfoCache.get(hash) : null;
      const result = {
        imageHash: hash,
        scaleMode: String(paint.scaleMode || "FILL"),
        opacity: finite(paint.opacity, 1),
      };
      if (typeof paint.scalingFactor === "number" && Number.isFinite(paint.scalingFactor))
        result.scalingFactor = paint.scalingFactor;
      if (typeof paint.rotation === "number" && Number.isFinite(paint.rotation))
        result.rotation = paint.rotation;
      if (paint.imageTransform) result.imageTransform = paint.imageTransform;
      if (paint.filters) result.filters = paint.filters;
      if (info && info.width > 0 && info.height > 0) {
        result.sourceWidth = info.width;
        result.sourceHeight = info.height;
      }
      return result;
    });
  }

  const mainComponentNode = instanceMainComponent(node);
  const componentKey = safeNodeValue(node, "key", "") ||
    safeNodeValue(mainComponentNode, "key", "");
  if (componentKey) providerData.componentKey = String(componentKey);
  const componentSet = node.parent && node.parent.type === "COMPONENT_SET"
    ? node.parent
    : (mainComponentNode && mainComponentNode.parent &&
       mainComponentNode.parent.type === "COMPONENT_SET"
      ? mainComponentNode.parent : null);
  if (componentSet && componentSet.id) {
    providerData.componentSetId = String(componentSet.id);
    // The set itself owns shared component-property definitions, but unrelated variants do not
    // affect the selected instance. Watch the set id plus the active main-component subtree
    // instead of walking every variant for every export.
    if (exportContext) exportContext.dependencyIds.add(providerData.componentSetId);
  }

  const mainComponent = instanceMainComponentId(node);
  if (mainComponent) {
    providerData.mainComponentId = mainComponent;
    if (exportContext) exportContext.dependencyIds.add(mainComponent);
    rememberDependencyTree(mainComponentNode);
  }

  // variantProperties is deprecated on InstanceNode. Modern Figma exposes variant values in
  // componentProperties; keep variantProperties only for ComponentNode variants.
  const variants = node.type === "INSTANCE"
    ? null : safeNodeValue(node, "variantProperties", null);
  if (variants && typeof variants === "object") providerData.variantProperties = variants;
  if (node.type === "INSTANCE") {
    const overrides = safeNodeValue(node, "overrides", null);
    if (Array.isArray(overrides) && overrides.length > 0) {
      providerData.overrides = overrides.map((entry) => ({
        id: String(entry && entry.id || ""),
        overriddenFields: Array.isArray(entry && entry.overriddenFields)
          ? Array.from(entry.overriddenFields, String) : [],
      }));
    }
    const exposedInstances = safeNodeValue(node, "exposedInstances", null);
    if (Array.isArray(exposedInstances) && exposedInstances.length > 0) {
      providerData.exposedInstanceIds = exposedInstances
        .filter((entry) => entry && entry.id).map((entry) => String(entry.id));
    }
    if (safeNodeValue(node, "isExposedInstance", false) === true)
      providerData.isExposedInstance = true;
    const scaleFactor = safeNodeValue(node, "scaleFactor", 1);
    if (typeof scaleFactor === "number" && Number.isFinite(scaleFactor) &&
        Math.abs(scaleFactor - 1) > 0.000001) {
      providerData.scaleFactor = scaleFactor;
    }
  }

  const componentProperties = safeNodeValue(node, "componentProperties", null);
  if (componentProperties && typeof componentProperties === "object") {
    providerData.componentProperties = componentProperties;
    const variantSelection = {};
    for (const [name, property] of Object.entries(componentProperties)) {
      if (property && property.type === "VARIANT" && "value" in property)
        variantSelection[name] = property.value;
    }
    if (Object.keys(variantSelection).length > 0)
      providerData.variantSelection = variantSelection;
  }
  const propertyReferences = safeNodeValue(node, "componentPropertyReferences", null);
  if (propertyReferences && typeof propertyReferences === "object") {
    providerData.componentPropertyReferences = propertyReferences;
  }
  const definitions = safeNodeValue(node, "componentPropertyDefinitions", null) ||
    safeNodeValue(mainComponentNode, "componentPropertyDefinitions", null) ||
    safeNodeValue(componentSet, "componentPropertyDefinitions", null);
  if (definitions && typeof definitions === "object") {
    providerData.componentDefinitions = compactComponentDefinitions(definitions);
  }
  attribute(lines, depth, "source-provider-data", slugString(metadataJson(providerData)));
  attribute(lines, depth, "fidelity", fidelity);
}

function effectiveLayoutSizing(node, axis, parentMode) {
  const property = axis === "horizontal" ? "layoutSizingHorizontal" : "layoutSizingVertical";
  const direct = String(safeNodeValue(node, property, "") || "").toUpperCase();
  if (direct === "FIXED" || direct === "HUG" || direct === "FILL") return direct;

  // Compatibility path and semantic fallback. Figma's shorthand sizing properties map onto
  // layoutGrow/layoutAlign for children and primary/counterAxisSizingMode for auto-layout
  // containers. Reading both keeps exporter behavior correct across host/API revisions.
  const parentHorizontal = parentMode === "HORIZONTAL";
  const parentVertical = parentMode === "VERTICAL";
  if (parentHorizontal || parentVertical) {
    const mainAxis = (parentHorizontal && axis === "horizontal") ||
      (parentVertical && axis === "vertical");
    if (mainAxis && finite(safeNodeValue(node, "layoutGrow", 0), 0) > 0.5) return "FILL";
    if (!mainAxis && String(safeNodeValue(node, "layoutAlign", "INHERIT")) === "STRETCH")
      return "FILL";
  }

  const ownMode = String(safeNodeValue(node, "layoutMode", "NONE"));
  if (ownMode === "HORIZONTAL" || ownMode === "VERTICAL") {
    const primaryAxis = (ownMode === "HORIZONTAL" && axis === "horizontal") ||
      (ownMode === "VERTICAL" && axis === "vertical");
    const sizingMode = String(safeNodeValue(
      node, primaryAxis ? "primaryAxisSizingMode" : "counterAxisSizingMode", "FIXED"));
    if (sizingMode === "AUTO") return "HUG";
  }
  return "FIXED";
}

function emitSizeAndPosition(node, kind, parentMode, isRoot, lines, depth, parentBounds, bounds) {
  const flowParent = parentMode === "HORIZONTAL" || parentMode === "VERTICAL";
  const absoluteInFlow = !isRoot && flowParent &&
    String(safeNodeValue(node, "layoutPositioning", "AUTO")) === "ABSOLUTE";

  if (!isRoot && (parentMode === "NONE" || absoluteInFlow)) {
    attribute(lines, depth, "x", number(bounds.x - finite(parentBounds && parentBounds.x, 0)) + "px");
    attribute(lines, depth, "y", number(bounds.y - finite(parentBounds && parentBounds.y, 0)) + "px");
  }
  if (absoluteInFlow) attribute(lines, depth, "position", "absolute");

  if (!isRoot) {
    const constraints = safeNodeValue(node, "constraints", null);
    if (constraints && typeof constraints === "object") {
      const values = { MIN: "min", CENTER: "center", MAX: "max", STRETCH: "stretch", SCALE: "scale" };
      if (values[String(constraints.horizontal || "")])
        attribute(lines, depth, "constraint-horizontal", values[String(constraints.horizontal)]);
      if (values[String(constraints.vertical || "")])
        attribute(lines, depth, "constraint-vertical", values[String(constraints.vertical)]);
    }
  }

  const horizontal = effectiveLayoutSizing(node, "horizontal", parentMode);
  const vertical = effectiveLayoutSizing(node, "vertical", parentMode);
  const renderedWidth = kind === "Shape" ? bounds.width : finite(node.width, bounds.width);
  const renderedHeight = kind === "Shape" ? bounds.height : finite(node.height, bounds.height);

  // Preserve Figma's already-resolved HUG extent as the intrinsic preferred size. SlugUI
  // otherwise has to re-measure text with a backend-generic estimate, which compounds into
  // visibly shifted component/button layouts. FILL stays responsive; vector FILL additionally
  // needs its authored owner extent so path placement can be normalized without fixing the axis.
  if (!isRoot) {
    if (horizontal === "HUG" || (kind === "Shape" && horizontal !== "FIXED"))
      attribute(lines, depth, "preferred-width", number(renderedWidth) + "px");
    if (vertical === "HUG" || (kind === "Shape" && vertical !== "FIXED"))
      attribute(lines, depth, "preferred-height", number(renderedHeight) + "px");
  }

  // Figma absolute children are snapshots inside auto-layout: they neither grow nor stretch.
  if (isRoot || absoluteInFlow || horizontal === "FIXED") {
    attribute(lines, depth, "width", number(renderedWidth) + "px");
  } else if (horizontal === "FILL" && parentMode === "HORIZONTAL") {
    attribute(lines, depth, "grow", "1");
  }
  if (isRoot || absoluteInFlow || vertical === "FIXED") {
    attribute(lines, depth, "height", number(renderedHeight) + "px");
  } else if (vertical === "FILL" && parentMode === "VERTICAL") {
    attribute(lines, depth, "grow", "1");
  }

  // Cross-axis FILL is an item override, not a property of the parent's alignment.
  if (!absoluteInFlow &&
      ((parentMode === "HORIZONTAL" && vertical === "FILL") ||
       (parentMode === "VERTICAL" && horizontal === "FILL"))) {
    attribute(lines, depth, "align-self", "stretch");
  }

  const limits = [
    ["minWidth", "min-width"], ["minHeight", "min-height"],
    ["maxWidth", "max-width"], ["maxHeight", "max-height"],
  ];
  for (const [property, name] of limits) {
    const value = safeNodeValue(node, property, null);
    if (typeof value === "number" && Number.isFinite(value) && value >= 0) {
      attribute(lines, depth, name, number(value) + "px");
    }
  }
}

function emitContainerLayout(node, kind, lines, depth) {
  const layout = containerLayout(node);
  const rawLayout = String(safeNodeValue(node, "layoutMode", "NONE"));
  if (rawLayout === "GRID") {
    diagnostic(node, "W_GRID_LAYOUT",
      "Figma grid auto-layout is emitted as an exact-position snapshot until grid IR is available.");
  } else if ((rawLayout === "HORIZONTAL" || rawLayout === "VERTICAL") && layout === "absolute") {
    if (String(safeNodeValue(node, "counterAxisAlignItems", "")) === "BASELINE") {
      diagnostic(node, "W_BASELINE_LAYOUT",
        "Figma baseline alignment is emitted as an exact-position snapshot to avoid baseline drift.");
    }
    if (safeNodeValue(node, "strokesIncludedInLayout", false) === true) {
      diagnostic(node, "W_STROKE_LAYOUT",
        "Figma stroke-inclusive auto-layout is emitted as an exact-position snapshot.");
    }
  }
  if (kind === "Rectangle") attribute(lines, depth, "layout", layout);
  if (layout === "row" || layout === "column") {
    if (safeNodeValue(node, "itemReverseZIndex", false) === true) {
      attribute(lines, depth, "reverse-paint-order", "true");
    }
    const wrapped = String(safeNodeValue(node, "layoutWrap", "NO_WRAP")) === "WRAP";
    if (wrapped) {
      attribute(lines, depth, "wrap", "true");
      const counterSpacing = safeNodeValue(node, "counterAxisSpacing", null);
      if (typeof counterSpacing === "number" && Number.isFinite(counterSpacing)) {
        attribute(lines, depth, "counter-spacing", number(counterSpacing) + "px");
      }
      if (String(safeNodeValue(node, "counterAxisAlignContent", "AUTO")) === "SPACE_BETWEEN") {
        attribute(lines, depth, "counter-justify", "space-between");
      }
    }
    const padding = [
      finite(node.paddingLeft, 0),
      finite(node.paddingTop, 0),
      finite(node.paddingRight, 0),
      finite(node.paddingBottom, 0),
    ];
    if (padding.some((value) => value !== 0)) {
      attribute(lines, depth, "padding",
        "[" + padding.map((value) => number(value) + "px").join(", ") + "]");
    }
    if (typeof node.itemSpacing === "number") {
      attribute(lines, depth, "spacing", number(node.itemSpacing) + "px");
    } else if (node.itemSpacing === "AUTO") {
      attribute(lines, depth, "justify", "space-between");
    }
    const justify = {
      MIN: "start", CENTER: "center", MAX: "end", SPACE_BETWEEN: "space-between",
      SPACE_AROUND: "space-around", SPACE_EVENLY: "space-evenly",
    };
    const align = { MIN: "start", CENTER: "center", MAX: "end", BASELINE: "start" };
    if (justify[node.primaryAxisAlignItems]) {
      attribute(lines, depth, "justify", justify[node.primaryAxisAlignItems]);
    }
    if (align[node.counterAxisAlignItems]) {
      attribute(lines, depth, "cross-align", align[node.counterAxisAlignItems]);
    }
  }
  if (node.clipsContent === true) attribute(lines, depth, "clip", "true");
}

function emitCorners(node, lines, depth) {
  if (!("cornerRadius" in node)) return;
  if (!isMixed(node.cornerRadius) && typeof node.cornerRadius === "number") {
    if (node.cornerRadius !== 0) attribute(lines, depth, "radius", number(node.cornerRadius));
  } else {
    const radii = [
      finite(node.topLeftRadius, 0),
      finite(node.topRightRadius, 0),
      finite(node.bottomRightRadius, 0),
      finite(node.bottomLeftRadius, 0),
    ];
    if (radii.some((radius) => radius !== 0)) {
      attribute(lines, depth, "radius", "[" + radii.map(number).join(", ") + "]");
    }
  }
  if (typeof node.cornerSmoothing === "number" && node.cornerSmoothing !== 0) {
    attribute(lines, depth, "smoothing", number(node.cornerSmoothing * 100) + "%");
  }
}

function emitStroke(node, lines, depth) {
  const strokes = visibleStrokes(node);
  if (strokes.length === 0) return;
  attribute(lines, depth, "stroke", strokePaintSource(node));

  let width = typeof node.strokeWeight === "number" ? node.strokeWeight : 1;
  if (isMixed(node.strokeWeight)) {
    const sides = [
      finite(node.strokeTopWeight, 0),
      finite(node.strokeRightWeight, 0),
      finite(node.strokeBottomWeight, 0),
      finite(node.strokeLeftWeight, 0),
    ];
    attribute(lines, depth, "stroke-width",
      "[" + sides.map(number).join(", ") + "]");
  } else {
    attribute(lines, depth, "stroke-width", number(Math.max(0, width)));
  }
  const align = { INSIDE: "inside", CENTER: "center", OUTSIDE: "outside" };
  attribute(lines, depth, "stroke-align", align[node.strokeAlign] || "center");
}

function inferredTextWeight(value, fallback) {
  if (typeof value.fontWeight === "number") return value.fontWeight;
  const fontName = value.fontName && !isMixed(value.fontName) ? value.fontName : null;
  const variations = fontName && fontName.variationSettings;
  if (variations && typeof variations.wght === "number" && Number.isFinite(variations.wght))
    return variations.wght;
  const style = String(fontName && fontName.style || "");
  if (/thin/i.test(style)) return 100;
  if (/extra.?light|ultra.?light/i.test(style)) return 200;
  if (/light/i.test(style)) return 300;
  if (/medium/i.test(style)) return 500;
  if (/semi.?bold|demi.?bold/i.test(style)) return 600;
  if (/extra.?bold|ultra.?bold/i.test(style)) return 800;
  if (/black|heavy/i.test(style)) return 900;
  if (/bold/i.test(style)) return 700;
  return fallback;
}

function textStyleItalic(value) {
  const fontName = value.fontName && !isMixed(value.fontName) ? value.fontName : null;
  const variations = fontName && fontName.variationSettings;
  if (variations) {
    if (typeof variations.ital === "number" && variations.ital >= 0.5) return true;
    if (typeof variations.slnt === "number" && Math.abs(variations.slnt) > 0.0001) return true;
  }
  return value.fontStyle === "ITALIC" ||
    /italic|oblique/i.test(String(fontName && fontName.style || ""));
}

function textLetterSpacing(value, fontSize) {
  if (!value || isMixed(value)) return 0;
  const spacing = finite(value.value, 0);
  return value.unit === "PERCENT" ? fontSize * spacing / 100 : spacing;
}

function textLineHeight(value, fontSize) {
  if (!value || isMixed(value) || value.unit === "AUTO") return 1.25;
  if (value.unit === "PERCENT") return finite(value.value, 125) / 100;
  if (value.unit === "PIXELS" && fontSize > 0) return finite(value.value, fontSize) / fontSize;
  return 1.25;
}

function casedText(text, textCase) {
  if (textCase === "UPPER" || textCase === "SMALL_CAPS" ||
      textCase === "SMALL_CAPS_FORCED") return text.toUpperCase();
  if (textCase === "LOWER") return text.toLowerCase();
  if (textCase === "TITLE") {
    return text.replace(/(^|\s)(\S)/gu, (match, prefix, character) =>
      prefix + character.toUpperCase());
  }
  return text;
}

function textStyleSignature(node) {
  const fontName = node.fontName && !isMixed(node.fontName)
    ? String(node.fontName.family || "") + "/" + String(node.fontName.style || "")
    : "mixed";
  const variationSettings = node.fontName && !isMixed(node.fontName) &&
    node.fontName.variationSettings ? metadataJson(node.fontName.variationSettings) : "";
  return [
    String(node.characters || ""), fontName, variationSettings,
    typeof node.fontSize === "number" ? node.fontSize : "mixed",
    typeof node.fontWeight === "number" ? node.fontWeight : "mixed",
    String(node.textDecoration || ""), String(node.textCase || ""),
  ].join("~");
}

function styledTextSegments(node) {
  if (typeof node.getStyledTextSegments !== "function") return null;
  const cacheKey = String(node.id || "");
  const cached = cacheKey ? styledTextCache.get(cacheKey) : null;
  const signature = cacheKey && !targetedNodeChangeEvents ? textStyleSignature(node) : "";
  if (cached && (targetedNodeChangeEvents || cached.signature === signature)) {
    if (exportContext) exportContext.textCacheHits += 1;
    return cached.segments;
  }
  try {
    const segments = node.getStyledTextSegments([
      "fontSize", "fontName", "fontWeight", "fontStyle", "textDecoration",
      "textDecorationStyle", "textCase", "lineHeight", "letterSpacing", "fills",
      "paragraphIndent", "paragraphSpacing", "listOptions", "listSpacing",
      "indentation", "openTypeFeatures",
    ]);
    if (cacheKey) styledTextCache.set(cacheKey, { signature, segments });
    return segments;
  } catch (error) {
    diagnostic(node, "E_STYLED_TEXT",
      "Figma could not expose styled text segments: " +
        (error && error.message ? error.message : error), "Unsupported");
    return null;
  }
}

function textRunSource(node, segment) {
  const fontSize = finite(segment.fontSize, finite(node.fontSize, 14));
  const fontName = segment.fontName && typeof segment.fontName.family === "string"
    ? segment.fontName.family : "system-ui";
  const weight = Math.max(1, Math.min(1000, inferredTextWeight(segment, 400)));
  const decoration = String(segment.textDecoration || "NONE");
  const paints = Array.isArray(segment.fills)
    ? segment.fills.filter((paint) => paint && paint.visible !== false)
    : visiblePaints(node);
  const fontStyle = segment.fontName && typeof segment.fontName.style === "string"
    ? segment.fontName.style : String(segment.fontStyle || "");
  return "text-run(" + [
    slugString(casedText(String(segment.characters || ""), segment.textCase)),
    slugString(fontName),
    number(fontSize),
    number(weight),
    String(textStyleItalic(segment)),
    String(decoration === "UNDERLINE"),
    String(decoration === "STRIKETHROUGH"),
    number(textLetterSpacing(segment.letterSpacing, fontSize)),
    number(textLineHeight(segment.lineHeight, fontSize)),
    paintListSource(node, paints, "fill"),
    slugString(fontStyle),
  ].join(", ") + ")";
}

function emitText(node, lines, depth, segments, textProperty) {
  const runOnlyStyle = segments && segments.some((segment) =>
    segment.textCase && segment.textCase !== "ORIGINAL");
  const rich = segments && segments.length > 0 &&
    (segments.length > 1 || isMixed(node.fontName) || isMixed(node.fontSize) ||
     isMixed(node.fontWeight) || isMixed(node.textDecoration) || isMixed(node.fills) ||
     isMixed(node.lineHeight) || isMixed(node.letterSpacing) || runOnlyStyle);
  const renderedText = rich
    ? segments.map((segment) =>
      casedText(String(segment.characters || ""), segment.textCase)).join("")
    : String(node.characters || "");
  if (textProperty && !rich) {
    attribute(lines, depth, "text", "$" + textProperty);
  } else {
    attribute(lines, depth, "text", slugString(renderedText));
    if (textProperty && rich) {
      diagnostic(node, "W_COMPONENT_TEXT_RICH",
        "A TEXT component property reference on mixed/rich text stays at its resolved snapshot until dynamic styled runs are supported.");
    }
  }
  let italic = false;
  let weight = inferredTextWeight(node, 400);
  if (typeof node.fontSize === "number") {
    attribute(lines, depth, "font-size", number(node.fontSize));
  } else if (isMixed(node.fontSize) && !rich) {
    diagnostic(node, "W_MIXED_TEXT", "Mixed text sizes are flattened to the first/default run.");
  }
  if (node.fontName && !isMixed(node.fontName) && typeof node.fontName.family === "string") {
    attribute(lines, depth, "font", slugString(node.fontName.family));
    const style = String(node.fontName.style || "");
    if (style) attribute(lines, depth, "font-style", slugString(style));
    italic = /italic|oblique/i.test(style);
  } else if (isMixed(node.fontName) && !rich) {
    diagnostic(node, "W_MIXED_FONT", "Mixed font families are flattened to the default font.");
  }
  if (isMixed(node.fontWeight) && !rich) {
    diagnostic(node, "W_MIXED_FONT_WEIGHT",
      "Mixed text weights require separate text runs and currently use weight 400.");
  }
  attribute(lines, depth, "font-weight", number(Math.max(1, Math.min(1000, weight))));
  if (italic) attribute(lines, depth, "italic", "true");
  if (node.textDecoration === "UNDERLINE") attribute(lines, depth, "underline", "true");
  if (node.textDecoration === "STRIKETHROUGH") attribute(lines, depth, "strikethrough", "true");
  const alignment = {
    LEFT: "left", CENTER: "center", RIGHT: "right", JUSTIFIED: "justify",
  };
  if (alignment[node.textAlignHorizontal]) {
    attribute(lines, depth, "text-align", alignment[node.textAlignHorizontal]);
  }
  const verticalAlignment = { TOP: "top", CENTER: "center", BOTTOM: "bottom" };
  if (verticalAlignment[node.textAlignVertical]) {
    attribute(lines, depth, "text-align-vertical", verticalAlignment[node.textAlignVertical]);
  }
  if (node.lineHeight && !isMixed(node.lineHeight)) {
    if (node.lineHeight.unit === "PERCENT") {
      attribute(lines, depth, "line-height", number(node.lineHeight.value / 100));
    } else if (node.lineHeight.unit === "PIXELS" && typeof node.fontSize === "number" && node.fontSize > 0) {
      attribute(lines, depth, "line-height", number(node.lineHeight.value / node.fontSize));
    }
  }
  if (node.letterSpacing && !isMixed(node.letterSpacing)) {
    let spacing = finite(node.letterSpacing.value, 0);
    if (node.letterSpacing.unit === "PERCENT" && typeof node.fontSize === "number") {
      spacing = node.fontSize * spacing / 100;
    }
    if (spacing !== 0) attribute(lines, depth, "letter-spacing", number(spacing));
  }
  const paragraphIndent = resolvedParagraphIndent(node, segments);
  if (paragraphIndent !== null && paragraphIndent !== 0) {
    attribute(lines, depth, "indent", number(paragraphIndent));
  }
  const listType = resolvedTextListType(node, segments);
  if (listType === "UNORDERED") {
    attribute(lines, depth, "list-marker", "bullet");
  } else if (listType === "ORDERED") {
    attribute(lines, depth, "list-marker", "numbered");
  }
  if (rich && styledTextNeedsApproximation(node, segments)) {
    diagnostic(node, "W_RICH_TEXT_ADVANCED",
      "Paragraph spacing, nested lists, decoration variants, or OpenType overrides need a richer text layout service.");
  }
  const basePaint = rich && segments.length > 0
    ? paintListSource(node, Array.isArray(segments[0].fills)
      ? segments[0].fills.filter((paint) => paint && paint.visible !== false) : [], "fill")
    : paintSource(node);
  attribute(lines, depth, "fill", basePaint);
  if (rich) {
    attribute(lines, depth, "text-runs",
      "[" + segments.map((segment) => textRunSource(node, segment)).join(", ") + "]");
  }
}

function emitImagePlaceholder(node, lines, depth) {
  const fills = ("fills" in node && !isMixed(node.fills) && Array.isArray(node.fills))
    ? node.fills : [];
  const paint = fills.find((candidate) =>
    candidate && candidate.visible !== false && candidate.type === "IMAGE");
  if (!paint) return;
  const hash = String(paint.imageHash || "");
  const info = hash ? imageInfoCache.get(hash) : null;
  if (info && info.width > 0 && info.height > 0) {
    attribute(lines, depth, "image-source-width", number(info.width));
    attribute(lines, depth, "image-source-height", number(info.height));
  }
  const mode = String(paint.scaleMode || "FILL").toLowerCase();
  if (["fill", "fit", "crop", "tile"].includes(mode))
    attribute(lines, depth, "image-scale-mode", mode);
}

function emitNode(node, parentMode, isRoot, lines, depth, parentBounds) {
  if (exportContext) {
    exportContext.nodeCount += 1;
    if (node && node.id) exportContext.nodeIds.add(String(node.id));
  }
  const kind = nodeKind(node);
  const vectorGeometry = kind === "Shape" ? bakedVectorGeometry(node) : null;
  const textSegments = kind === "Text" ? styledTextSegments(node) : null;
  const bounds = exportBounds(node);
  const fidelity = fidelityFor(node, vectorGeometry, textSegments);
  const fidelityCount = fidelity === "baked-vector" ? "bakedVector" : fidelity;
  fidelityCounts[fidelityCount] += 1;
  push(lines, depth, kind + " " + elementName(node) + " {");
  emitSizeAndPosition(node, kind, parentMode, isRoot, lines, depth + 1, parentBounds, bounds);
  const visibleProperty = runtimePropertyReference(node, "visible", "BOOLEAN");
  if (visibleProperty) attribute(lines, depth + 1, "visible", "$" + visibleProperty);

  if (kind === "Rectangle" || ["Row", "Column", "Absolute"].includes(kind)) {
    emitContainerLayout(node, kind, lines, depth + 1);
  }
  if (kind === "Rectangle") {
    attribute(lines, depth + 1, "fill", paintSource(node));
    emitImagePlaceholder(node, lines, depth + 1);
    emitCorners(node, lines, depth + 1);
    emitStroke(node, lines, depth + 1);
  } else if (kind === "Text") {
    emitText(node, lines, depth + 1, textSegments,
      runtimePropertyReference(node, "characters", "TEXT"));
  } else if (kind === "Shape") {
    const ownerBounds = bounds;
    const fillPaths = vectorGeometry ? vectorGeometry.fillPaths : [];
    const strokePaths = vectorGeometry ? vectorGeometry.strokePaths : [];
    if (fillPaths.length > 0) {
      attribute(lines, depth + 1, "path-data", pathDataValue(fillPaths));
      attribute(lines, depth + 1, "path-winding-rules", pathWindingValue(fillPaths));
      const placement = placementSource(vectorGeometry.fillBounds, ownerBounds);
      if (placement) attribute(lines, depth + 1, "path-placement", placement);
      attribute(lines, depth + 1, "fill", paintSource(node));
    }
    if (strokePaths.length > 0) {
      attribute(lines, depth + 1, "stroke-path-data", pathDataValue(strokePaths));
      attribute(lines, depth + 1, "stroke-winding-rules", pathWindingValue(strokePaths));
      const placement = placementSource(vectorGeometry.strokeBounds, ownerBounds);
      if (placement) attribute(lines, depth + 1, "stroke-placement", placement);
      emitStroke(node, lines, depth + 1);
    }
    if (fillPaths.length === 0 && strokePaths.length === 0) {
      attribute(lines, depth + 1, "shape", "0");
      attribute(lines, depth + 1, "fill", paintSource(node));
      diagnostic(node, "E_VECTOR_GEOMETRY_MISSING",
        "Figma exposed no renderable fillGeometry or strokeGeometry; a placeholder was emitted.",
        "Unsupported");
    }
  }

  if (node.effects && !isMixed(node.effects) &&
      node.effects.some((effect) => effect.visible !== false)) {
    diagnostic(node, "W_EFFECTS", "Blur and shadow effects are not represented by the current IR.");
  }
  if (kind !== "Rectangle" && hasVisualStroke(node) &&
      (!vectorGeometry || vectorGeometry.strokePaths.length === 0)) {
    diagnostic(node, "W_STROKES",
      "Figma exposed no strokeGeometry, so this node's stroke paint cannot be rendered.");
  }
  if (finite(node.opacity, 1) < 0.9995 && childrenOf(node).length > 0) {
    diagnostic(node, "W_GROUP_OPACITY",
      "Container opacity is baked into its own fill but does not yet isolate descendant compositing.");
  }
  if (hasNonIdentityAxes(safeNodeValue(node, "relativeTransform", null))) {
    diagnostic(node, "W_TRANSFORM",
      "Rotation/skew transform metadata is preserved, but current SlugUI layout remains axis-aligned.");
  }
  const transformModifiers = safeNodeValue(node, "transformModifiers", null);
  if (Array.isArray(transformModifiers) && transformModifiers.length > 0) {
    diagnostic(node, "W_TRANSFORM_MODIFIERS",
      "Transform-group modifiers are preserved in source metadata; repeated copies are not expanded yet.");
  }
  if (![
    "TEXT", "RECTANGLE", "FRAME", "COMPONENT", "INSTANCE", "COMPONENT_SET", "GROUP", "SECTION",
    "SLOT", "TRANSFORM_GROUP", "VECTOR", "BOOLEAN_OPERATION", "ELLIPSE", "POLYGON", "STAR", "LINE",
  ].includes(node.type)) {
    diagnostic(node, "E_NODE_TYPE", "Unsupported node type " + node.type + " became an Absolute container.", "Unsupported");
  }
  emitSourceMetadata(node, lines, depth + 1, fidelity);

  const exportedLayout = containerLayout(node);
  const childParentMode = exportedLayout === "row"
    ? "HORIZONTAL" : exportedLayout === "column" ? "VERTICAL" : "NONE";
  for (const child of childrenOf(node)) {
    emitNode(child, childParentMode, false, lines, depth + 1, bounds);
  }
  push(lines, depth, "}");
}

function buildSelectionExport(selection) {
  diagnostics.length = 0;
  fidelityCounts.native = 0;
  fidelityCounts.approximated = 0;
  fidelityCounts.bakedVector = 0;
  fidelityCounts.unsupported = 0;
  if (!selection || selection.length === 0) {
    return {
      source: "",
      filename: "",
      component: "",
      diagnostics: [{
        code: "E_NO_SELECTION", nodeId: "", nodeName: "",
        message: "Select one Frame, Component, or node to export.", fidelity: "Unsupported",
      }],
      fidelityCounts: { ...fidelityCounts },
    };
  }
  // PageNode.selection ordering is explicitly unspecified. Preserve Figma painter order for
  // overlapping multi-selection exports by sorting through each node's document-tree index path.
  selection = topLevelSelection(Array.from(selection));
  // emitNode()/childrenOf() collect synchronization IDs while generating the source. This keeps
  // export traversal single-pass: every visible branch is walked once, and invisible direct
  // children are still watched so making them visible invalidates the selected container.
  if (selection.length > 1) selection.sort(compareSceneOrder);
  const component = componentName(selection);
  const lines = [
    "// SlugUI exchange format " + FORMAT_VERSION + ", generated by the Figma selection exporter.",
    "component " + component + " {",
  ];
  const liveProperties = runtimeComponentProperties(selection);
  for (const property of liveProperties) {
    const type = property.type === "BOOLEAN" ? "bool" : "string";
    const value = property.type === "BOOLEAN"
      ? (property.value ? "true" : "false") : slugString(property.value);
    push(lines, 1, "property " + type + " " + property.identifier + " = " + value + ";");
  }
  const bounds = selection.map((node) => nodeBounds(node, true));
  const left = Math.min(...bounds.map((value) => value.x));
  const top = Math.min(...bounds.map((value) => value.y));
  const right = Math.max(...bounds.map((value) => value.x + value.width));
  const bottom = Math.max(...bounds.map((value) => value.y + value.height));
  push(lines, 1, "Absolute selection_root {");
  attribute(lines, 2, "width", number(right - left) + "px");
  attribute(lines, 2, "height", number(bottom - top) + "px");
  for (const node of selection) {
    emitNode(node, "NONE", false, lines, 2,
      { x: left, y: top, width: right - left, height: bottom - top });
  }
  push(lines, 1, "}");
  lines.push("}", "");
  return {
    source: lines.join("\n"),
    filename: component + ".slugui",
    component,
    diagnostics: diagnostics.slice(),
    fidelityCounts: { ...fidelityCounts },
  };
}

function exportSelection(selection) {
  const started = Date.now();
  exportContext = createExportContext();
  try {
    const result = buildSelectionExport(selection);
    result.stats = {
      nodeCount: exportContext.nodeCount,
      vectorCacheHits: exportContext.vectorCacheHits,
      textCacheHits: exportContext.textCacheHits,
      generationMs: Math.max(0, Date.now() - started),
    };
    lastExportUsedTemporaryMutation = exportContext.temporaryMutation;
    lastExportNodeIds = exportContext.nodeIds;
    lastExportDependencyIds = exportContext.dependencyIds;
    return result;
  } finally {
    exportContext = null;
  }
}

let lastSelectionKey = "";
let refreshTimer = null;
let suppressDocumentChangesUntil = 0;
let lastExportUsedTemporaryMutation = false;
let lastExportNodeIds = new Set();
let lastExportDependencyIds = new Set();
let lastExportSource = "";
let lastExportFilename = "";
let refreshRevision = 0;
let observedPage = null;

function refreshSelection(force) {
  lastExportUsedTemporaryMutation = false;
  const revision = ++refreshRevision;
  const selection = Array.from(figma.currentPage.selection || []);
  const key = selectionKey(selection);
  if (!force && key === lastSelectionKey) return;
  lastSelectionKey = key;

  // Dynamic-page plugins should resolve InstanceNode.mainComponent through the async API.
  // Delay only selections that actually contain unresolved instances; all other exports stay
  // synchronous and keep the fast interaction path.
  const pendingComponents = primeMainComponents(selection);
  const pendingImages = primeImageInfo(selection);
  if (pendingComponents || pendingImages) {
    Promise.all([pendingComponents, pendingImages].filter(Boolean)).then(() => {
      if (revision === refreshRevision) refreshSelection(true);
    });
    return;
  }

  try {
    const result = exportSelection(selection);
    lastExportSource = result.source || "";
    lastExportFilename = result.filename || "";
    const inlineSource = lastExportSource.length <= INLINE_SOURCE_LIMIT;
    figma.ui.postMessage({
      type: "export-result",
      ...result,
      source: inlineSource ? lastExportSource : "",
      sourcePreview: inlineSource ? "" :
        lastExportSource.slice(0, INLINE_SOURCE_LIMIT) +
          "\n\n// … preview truncated; Copy/Download requests the complete .slugui on demand",
      sourceAvailable: lastExportSource.length > 0,
      sourceLength: lastExportSource.length,
      selectionCount: selection.length,
      formatVersion: FORMAT_VERSION,
    });
  } catch (error) {
    lastExportSource = "";
    lastExportFilename = "";
    figma.ui.postMessage({
      type: "export-result",
      source: "",
      filename: "",
      component: "",
      selectionCount: selection.length,
      formatVersion: FORMAT_VERSION,
      diagnostics: [{
        code: "E_EXPORT", nodeId: "", nodeName: "",
        message: error && error.message ? error.message : String(error),
        fidelity: "Unsupported",
      }],
      fidelityCounts: { native: 0, approximated: 0, bakedVector: 0, unsupported: 0 },
      stats: { nodeCount: 0, vectorCacheHits: 0, textCacheHits: 0, generationMs: 0 },
    });
  } finally {
    // outlineStroke() creates and removes a temporary Figma node. Ignore only the immediate
    // node-change churn caused by passes that actually sampled exact stroke geometry.
    if (lastExportUsedTemporaryMutation) {
      suppressDocumentChangesUntil = Date.now() + 120;
    }
  }
}

function nodeChangeSummary(event) {
  if (!event) return null;
  const list = Array.isArray(event.nodeChanges)
    ? event.nodeChanges
    : (Array.isArray(event.documentChanges) ? event.documentChanges : null);
  if (!list) return null;
  const ids = new Set();
  const createdIds = new Set();
  let structural = false;
  for (const change of list) {
    if (!change || typeof change !== "object") continue;
    const id = change.id ? String(change.id) : "";
    if (id) ids.add(id);
    if (change.node && change.node.id) ids.add(String(change.node.id));
    if (change.node && change.node.parent && change.node.parent.id) {
      ids.add(String(change.node.parent.id));
    }
    const type = String(change.type || "").toUpperCase();
    if (type === "CREATE" || type === "DELETE") structural = true;
    if (type === "CREATE" && id) createdIds.add(id);
  }
  return { ids, createdIds, structural };
}

function setIntersects(first, second) {
  for (const value of first) if (second.has(value)) return true;
  return false;
}

function invalidateCaches(ids) {
  for (const id of ids) {
    vectorGeometryCache.delete(id);
    styledTextCache.delete(id);
    mainComponentCache.delete(id);
  }
}

function touchesWatchedAncestor(node) {
  let current = node;
  let depth = 0;
  while (current && typeof current === "object" && depth++ < 256) {
    const id = current.id ? String(current.id) : "";
    if (id && (lastExportNodeIds.has(id) || lastExportDependencyIds.has(id))) return true;
    current = current.parent;
  }
  return false;
}

function resolveStructuralTouch(changes) {
  if (!changes || !changes.structural || changes.createdIds.size === 0 ||
      typeof figma.getNodeByIdAsync !== "function") return null;
  const pending = [];
  for (const id of changes.createdIds) {
    pending.push(Promise.resolve(figma.getNodeByIdAsync(id)).catch(() => null));
  }
  return Promise.all(pending).then((nodes) => nodes.some(touchesWatchedAncestor));
}

function queueDocumentRefresh(changes, dependencyHit, selectedHit) {
  if (Date.now() < suppressDocumentChangesUntil &&
      (!changes || (!dependencyHit && !selectedHit))) return;

  if (changes) {
    if (!dependencyHit && !selectedHit) return;
    if (dependencyHit || changes.structural) {
      // A definition edit or tree-shape edit can affect many descendants at once.
      invalidateCaches(lastExportNodeIds);
    } else {
      invalidateCaches(changes.ids);
    }
  } else {
    vectorGeometryCache.clear();
    styledTextCache.clear();
    mainComponentCache.clear();
  }

  const run = () => {
    refreshTimer = null;
    refreshSelection(true);
  };
  if (typeof setTimeout !== "function") {
    run();
    return;
  }
  if (refreshTimer !== null && typeof clearTimeout === "function") clearTimeout(refreshTimer);
  refreshTimer = setTimeout(run, 60);
}

function scheduleDocumentRefresh(event) {
  const changes = nodeChangeSummary(event);
  const dependencyHit = changes
    ? setIntersects(changes.ids, lastExportDependencyIds) : false;
  const selectedHit = changes ? setIntersects(changes.ids, lastExportNodeIds) : false;

  if (changes && !dependencyHit && !selectedHit) {
    // Figma's structural change payload is not identical across host versions. Some CREATE
    // records expose only the new id, not its parent. Resolve only those new nodes lazily and
    // walk their ancestors so adding a child to a selected Group/Component cannot be missed.
    const structuralTouch = resolveStructuralTouch(changes);
    if (structuralTouch) {
      structuralTouch.then((touches) => {
        if (touches) queueDocumentRefresh(changes, false, true);
      });
    }
    return;
  }
  queueDocumentRefresh(changes, dependencyHit, selectedHit);
}

function bindCurrentPageChanges() {
  const page = figma.currentPage;
  if (observedPage === page) {
    return Boolean(page && typeof page.on === "function");
  }
  if (observedPage && typeof observedPage.off === "function") {
    try { observedPage.off("nodechange", scheduleDocumentRefresh); } catch (_) {}
  }
  observedPage = page;
  targetedNodeChangeEvents = false;
  if (page && typeof page.on === "function") {
    try {
      page.on("nodechange", scheduleDocumentRefresh);
      targetedNodeChangeEvents = true;
      return true;
    } catch (_) {}
  }
  return false;
}

figma.showUI(__html__, { width: 580, height: 700, themeColors: true });
figma.ui.onmessage = (message) => {
  if (!message || message.type === "refresh") refreshSelection(true);
  if (message && message.type === "request-source") {
    const action = message.action === "download" ? "download" : "copy";
    figma.ui.postMessage({
      type: "source-payload",
      action,
      source: lastExportSource,
      filename: lastExportFilename,
    });
  }
  if (message && message.type === "close") figma.closePlugin();
};
figma.on("selectionchange", () => refreshSelection(false));
const pageNodeChanges = bindCurrentPageChanges();
if (!pageNodeChanges) {
  // Compatibility fallback for older hosts. Current dynamic-page Figma uses PageNode.nodechange,
  // avoiding the expensive loadAllPagesAsync() requirement of figma.documentchange.
  try { figma.on("documentchange", scheduleDocumentRefresh); } catch (_) {}
}
figma.on("currentpagechange", () => {
  bindCurrentPageChanges();
  lastSelectionKey = "";
  vectorGeometryCache.clear();
  styledTextCache.clear();
  mainComponentCache.clear();
  refreshSelection(true);
});
refreshSelection(true);
