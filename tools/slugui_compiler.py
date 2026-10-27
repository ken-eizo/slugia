#!/usr/bin/env python3
"""Compile the human-readable .slugui language into typed C++ SlugUI IR."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from svg_path import Bounds, SvgPathError, operation_bounds, parse_svg_path


class CompileError(Exception):
    def __init__(self, message: str, token: "Token | None" = None):
        super().__init__(message)
        self.message = message
        self.token = token


@dataclass(frozen=True, slots=True)
class Token:
    kind: str
    value: str
    offset: int
    source: str = field(repr=False, compare=False)

    @property
    def line(self) -> int:
        # Successful compiles never need source locations. Resolve them lazily only when a
        # diagnostic is actually formatted instead of updating line/column for every token.
        return self.source.count("\n", 0, self.offset) + 1

    @property
    def column(self) -> int:
        previous = self.source.rfind("\n", 0, self.offset)
        return self.offset + 1 if previous < 0 else self.offset - previous


class Lexer:
    # The original lexer advanced one Python character at a time. Figma exports are dominated by
    # metadata and SVG path strings, so that approach spent most of AOT time in _peek/_take.
    # One compiled scanner keeps the same grammar/error locations while moving the hot loop to C.
    _TOKEN_RE = re.compile(
        r'(?P<SPACE>\s+)'
        r'|(?P<LINE>//[^\r\n]*(?:\r?\n|$))'
        r'|(?P<BLOCK>/\*.*?\*/)'
        r'|(?P<STRING>"(?:\\[^\r\n]|[^"\\\r\n])*")'
        r'|(?P<COLOR>#[0-9A-Fa-f]*)'
        r'|(?P<NUMBER>[+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)'
        r'|(?P<IDENT>(?:[^\W\d]|_)[\w-]*)'
        r'|(?P<SYMBOL>[{}\[\]():;=,$%])',
        re.DOTALL | re.UNICODE,
    )

    def __init__(self, source: str):
        self.source = source

    def tokens(self) -> list[Token]:
        source = self.source
        length = len(source)
        position = 0
        result: list[Token] = []
        append = result.append
        match_token = self._TOKEN_RE.match

        while position < length:
            token_offset = position
            match = match_token(source, position)
            if match is None:
                if source.startswith("/*", position):
                    raise CompileError(
                        "unterminated block comment", Token("", "", token_offset, source))
                if source[position] == '"':
                    newline = source.find("\n", position + 1)
                    quote = source.find('"', position + 1)
                    message = ("newline in string literal"
                               if newline >= 0 and (quote < 0 or newline < quote)
                               else "unterminated string literal")
                    raise CompileError(message, Token("STRING", "", token_offset, source))
                current = source[position]
                raise CompileError(
                    f"unexpected character {current!r}",
                    Token("", current, token_offset, source))

            kind = match.lastgroup or ""
            raw = match.group(0)
            end = match.end()

            if kind == "NUMBER":
                # Preserve the old targeted diagnostics for partially-authored numeric literals.
                if end < length and source[end] == ".":
                    raise CompileError(
                        "expected digits after decimal point",
                        Token("NUMBER", raw + ".", token_offset, source))
                if end < length and source[end] in "eE":
                    tail = source[end]
                    if end + 1 < length and source[end + 1] in "+-":
                        tail += source[end + 1]
                    raise CompileError(
                        "invalid number exponent",
                        Token("NUMBER", raw + tail, token_offset, source))

            position = end
            if kind in ("SPACE", "LINE", "BLOCK"):
                continue

            if kind == "SYMBOL":
                append(Token(raw, raw, token_offset, source))
                continue
            if kind == "COLOR":
                digits = raw[1:]
                if len(digits) not in (6, 8):
                    raise CompileError(
                        "colors must use #RRGGBB or #RRGGBBAA",
                        Token("COLOR", digits, token_offset, source))
                append(Token("COLOR", digits.lower(), token_offset, source))
                continue
            if kind == "STRING":
                if "\\" not in raw:
                    decoded = raw[1:-1]
                else:
                    try:
                        decoded = json.loads(raw)
                    except json.JSONDecodeError as error:
                        raise CompileError(
                            f"invalid string escape: {error.msg}",
                            Token("STRING", "", token_offset, source)) from error
                append(Token("STRING", decoded, token_offset, source))
                continue
            append(Token(kind, raw, token_offset, source))

        append(Token("EOF", "", length, source))
        return result


@dataclass(slots=True)
class Value:
    kind: str
    value: object
    token: Token
    arguments: list["Value"] = field(default_factory=list)


@dataclass(slots=True)
class PropertyDecl:
    type_name: str
    name: str
    initial: Value
    token: Token


@dataclass(slots=True)
class Attribute:
    name: str
    value: Value
    token: Token


@dataclass(slots=True)
class ElementDecl:
    kind: str
    name: str
    attributes: list[Attribute]
    children: list["ElementDecl"]
    token: Token


@dataclass(slots=True)
class ComponentDecl:
    name: str
    properties: list[PropertyDecl]
    root: ElementDecl
    token: Token
    definitions: list["ComponentDecl"] = field(default_factory=list)


class Parser:
    PROPERTY_TYPES = {
        "bool", "int", "float", "string", "color", "paint", "length", "radii",
        "smoothing", "border-widths",
    }

    def __init__(self, tokens: list[Token]):
        self.tokens = tokens
        self.index = 0

    def _peek(self, offset: int = 0) -> Token:
        # The token stream always ends in EOF. Parser lookahead never steps beyond EOF, so a
        # bounds-clamping min() on every peek only adds overhead to the hottest parser path.
        return self.tokens[self.index + offset]

    def _take(self) -> Token:
        value = self.tokens[self.index]
        self.index += 1
        return value

    def _accept(self, kind: str, value: str | None = None) -> Token | None:
        token = self.tokens[self.index]
        if token.kind == kind and (value is None or token.value == value):
            self.index += 1
            return token
        return None

    def _expect(self, kind: str, value: str | None = None, message: str | None = None) -> Token:
        token = self._accept(kind, value)
        if token is None:
            expected = value if value is not None else kind
            raise CompileError(message or f"expected {expected}", self._peek())
        return token

    def parse(self) -> ComponentDecl:
        component_token = self._expect("IDENT", "component", "file must start with 'component'")
        name = self._expect("IDENT", message="expected component name").value
        component = self._parse_component_body(
            name, component_token, allow_definitions=True, kind_label="component")
        self._expect("EOF", message="only one exported component is allowed per file")
        return component

    def _parse_component_body(
            self, name: str, token: Token, *,
            allow_definitions: bool, kind_label: str) -> ComponentDecl:
        self._expect("{")
        properties: list[PropertyDecl] = []
        definitions: list[ComponentDecl] = []
        roots: list[ElementDecl] = []
        while not self._accept("}"):
            if self._peek().kind == "EOF":
                raise CompileError(f"unterminated {kind_label} '{name}'", token)
            if self._peek().kind == "IDENT" and self._peek().value == "property":
                properties.append(self._parse_property())
                continue
            if self._peek().kind == "IDENT" and self._peek().value == "definition":
                if not allow_definitions:
                    raise CompileError("nested definition declarations are not allowed", self._peek())
                definition_token = self._take()
                definition_name = self._expect(
                    "IDENT", message="expected definition name").value
                definitions.append(self._parse_component_body(
                    definition_name, definition_token,
                    allow_definitions=False, kind_label="definition"))
                continue
            roots.append(self._parse_element())

        if len(roots) != 1:
            raise CompileError(
                f"{kind_label} '{name}' must contain exactly one root element", token)
        property_names: set[str] = set()
        for prop in properties:
            if prop.name in property_names:
                raise CompileError(f"duplicate property '{prop.name}'", prop.token)
            property_names.add(prop.name)
        definition_names: set[str] = set()
        for definition in definitions:
            if definition.name in definition_names:
                raise CompileError(
                    f"duplicate definition '{definition.name}'", definition.token)
            definition_names.add(definition.name)
        return ComponentDecl(name, properties, roots[0], token, definitions)

    def _parse_property(self) -> PropertyDecl:
        token = self._expect("IDENT", "property")
        type_token = self._expect("IDENT", message="expected property type")
        if type_token.value not in self.PROPERTY_TYPES:
            raise CompileError(f"unknown property type '{type_token.value}'", type_token)
        name = self._expect("IDENT", message="expected property name").value
        self._expect("=")
        initial = self._parse_value()
        self._expect(";")
        return PropertyDecl(type_token.value, name, initial, token)

    def _parse_element(self) -> ElementDecl:
        kind = self._expect("IDENT", message="expected element kind")
        name = self._expect("IDENT", message="expected stable element name").value
        self._expect("{")
        attributes: list[Attribute] = []
        children: list[ElementDecl] = []
        attribute_names: set[str] = set()
        while not self._accept("}"):
            if self._peek().kind == "EOF":
                raise CompileError(f"unterminated element '{name}'", kind)
            if self._peek().kind != "IDENT":
                raise CompileError("expected attribute or child element", self._peek())
            if self._peek(1).kind == ":":
                attribute_token = self._take()
                self._take()
                if attribute_token.value in attribute_names:
                    raise CompileError(f"duplicate attribute '{attribute_token.value}'", attribute_token)
                attribute_names.add(attribute_token.value)
                attributes.append(Attribute(attribute_token.value, self._parse_value(), attribute_token))
                self._expect(";")
            else:
                children.append(self._parse_element())
        return ElementDecl(kind.value, name, attributes, children, kind)

    def _parse_value(self) -> Value:
        token = self._peek()
        if self._accept("$"):
            name = self._expect("IDENT", message="expected property name after '$'")
            return Value("reference", name.value, token)
        if value := self._accept("STRING"):
            return Value("string", value.value, value)
        if value := self._accept("COLOR"):
            return Value("color", value.value, value)
        if value := self._accept("NUMBER"):
            if self._peek().kind == "IDENT" and self._peek().value in ("px", "ppx"):
                unit = self._take().value
                return Value("length", (value.value, unit), value)
            if self._accept("%"):
                return Value("length", (value.value, "%"), value)
            return Value("number", value.value, value)
        if value := self._accept("IDENT"):
            if value.value in ("true", "false"):
                return Value("bool", value.value == "true", value)
            if self._accept("("):
                arguments: list[Value] = []
                if not self._accept(")"):
                    while True:
                        arguments.append(self._parse_value())
                        if self._accept(")"):
                            break
                        self._expect(",", message="expected ',' or ')' in function call")
                return Value("call", value.value, value, arguments)
            return Value("enum", value.value, value)
        if opening := self._accept("["):
            arguments: list[Value] = []
            if not self._accept("]"):
                while True:
                    arguments.append(self._parse_value())
                    if self._accept("]"):
                        break
                    self._expect(",", message="expected ',' or ']' in list")
            return Value("list", None, opening, arguments)
        raise CompileError("expected value", token)


def parse_source(source: str) -> ComponentDecl:
    return Parser(Lexer(source).tokens()).parse()


PROPERTY_CPP = {
    "bool": "bool",
    "int": "std::int64_t",
    "float": "float",
    "string": "std::string",
    "color": "slugia::Color",
    "paint": "slugia::Paint",
    "length": "slugia::slugui::Length",
    "radii": "slugia::CornerRadii",
    "smoothing": "slugia::CornerSmoothing",
    "border-widths": "slugia::BorderWidths",
}


def cpp_identifier(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if not result or result[0].isdigit():
        result = "_" + result
    if result in {
        "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch",
        "char", "class", "const", "constexpr", "continue", "default", "delete", "do",
        "double", "else", "enum", "explicit", "export", "extern", "false", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "namespace", "new",
        "noexcept", "not", "nullptr", "operator", "or", "private", "protected",
        "public", "register", "reinterpret_cast", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "template", "this", "throw", "true",
        "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
        "volatile", "while", "xor",
    }:
        result += "_"
    return result


def cpp_string(value: str) -> str:
    # Figma metadata, names, and SVG paths are overwhelmingly printable ASCII. Keep that hot
    # path in CPython's C-level replace implementation; only Unicode/control bytes need the
    # byte-by-byte octal escape path required for deterministic source encoding.
    if value.isascii() and (not value or value.isprintable()):
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

    escaped: list[str] = []
    for byte in value.encode("utf-8"):
        if byte == 0x22:
            escaped.append(r'\"')
        elif byte == 0x5C:
            escaped.append(r"\\")
        elif byte == 0x0A:
            escaped.append(r"\n")
        elif byte == 0x0D:
            escaped.append(r"\r")
        elif byte == 0x09:
            escaped.append(r"\t")
        elif 0x20 <= byte <= 0x7E:
            escaped.append(chr(byte))
        else:
            escaped.append(f"\\{byte:03o}")
    return '"' + "".join(escaped) + '"'


def cpp_string_chunks(value: str, max_utf8_bytes: int = 2048) -> list[str]:
    encoded_size = len(value.encode("utf-8"))
    if encoded_size <= max_utf8_bytes:
        return [cpp_string(value)]
    chunks: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for character in value:
        size = len(character.encode("utf-8"))
        if current and current_bytes + size > max_utf8_bytes:
            chunks.append(cpp_string("".join(current)))
            current.clear()
            current_bytes = 0
        current.append(character)
        current_bytes += size
    if current:
        chunks.append(cpp_string("".join(current)))
    return chunks


def compact_figma_provider_data(value: str) -> str:
    """Remove repeated discovery-only component catalogs from legacy Figma exports.

    Current exporter versions already emit preferredValueCount instead of duplicating every
    INSTANCE_SWAP candidate key on every instance. Older .slugui files may contain hundreds of
    preferredValues per node, bloating generated C++ without affecting the selected UI state.
    """
    if '"preferredValues"' not in value and '"componentProperties"' not in value:
        return value
    try:
        payload = json.loads(value)
    except (TypeError, ValueError, json.JSONDecodeError):
        return value
    if not isinstance(payload, dict):
        return value
    changed = False
    component_properties = payload.get("componentProperties")
    if isinstance(component_properties, dict) and "variantSelection" not in payload:
        variant_selection = {
            name: prop.get("value")
            for name, prop in component_properties.items()
            if isinstance(prop, dict) and prop.get("type") == "VARIANT" and "value" in prop
        }
        if variant_selection:
            payload["variantSelection"] = variant_selection
            changed = True

    definitions = payload.get("componentDefinitions")
    if not isinstance(definitions, dict):
        return (json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
                if changed else value)
    for definition in definitions.values():
        if not isinstance(definition, dict):
            continue
        preferred = definition.get("preferredValues")
        if isinstance(preferred, list):
            definition["preferredValueCount"] = len(preferred)
            del definition["preferredValues"]
            changed = True
    if not changed:
        return value
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def cpp_float(raw: str) -> str:
    if "e" not in raw.lower() and "." not in raw:
        raw += ".0"
    return raw + "f"


def number_text(value: Value, *, integer: bool = False) -> str:
    if value.kind != "number":
        raise CompileError("expected number", value.token)
    raw = str(value.value)
    if integer:
        try:
            parsed = float(raw)
        except ValueError as error:
            raise CompileError("invalid integer", value.token) from error
        if not parsed.is_integer():
            raise CompileError("integer property requires an integer literal", value.token)
        return str(int(parsed))
    return cpp_float(raw)


def color_cpp(value: Value) -> str:
    if value.kind != "color":
        raise CompileError("expected #RRGGBB or #RRGGBBAA color", value.token)
    digits = str(value.value)
    if len(digits) == 6:
        return f"slugia::Color::fromRgb8(0x{digits})"
    alpha = int(digits[6:8], 16) / 255.0
    return f"slugia::Color::fromRgb8(0x{digits[:6]}, {cpp_float(f'{alpha:.8g}')})"


def scalar_from_value(value: Value, *, allow_percent: bool = False) -> str:
    if value.kind == "number":
        return number_text(value)
    if allow_percent and value.kind == "length" and value.value[1] == "%":
        return cpp_float(str(value.value[0]))
    raise CompileError("expected scalar", value.token)


def length_cpp(value: Value) -> str:
    if value.kind == "enum" and value.value == "auto":
        return "slugia::slugui::Length::autoSize()"
    if value.kind == "number":
        return f"slugia::slugui::Length::logical({number_text(value)})"
    if value.kind != "length":
        raise CompileError("expected length (px, ppx, %, or auto)", value.token)
    raw, unit = value.value
    scalar = Value("number", raw, value.token)
    method = {"px": "logical", "ppx": "physical", "%": "percent"}[unit]
    return f"slugia::slugui::Length::{method}({number_text(scalar)})"


def numeric_list(value: Value, allowed_sizes: set[int], label: str,
                 allow_percent: bool = False) -> list[str]:
    values = value.arguments if value.kind == "list" else [value]
    if len(values) not in allowed_sizes:
        sizes = ", ".join(str(size) for size in sorted(allowed_sizes))
        raise CompileError(f"{label} expects {sizes} value(s)", value.token)
    return [scalar_from_value(item, allow_percent=allow_percent) for item in values]


def radii_cpp(value: Value) -> str:
    values = numeric_list(value, {1, 4}, "radius")
    if len(values) == 1:
        return f"slugia::CornerRadii::all({values[0]})"
    return "slugia::CornerRadii{" + ", ".join(values) + "}"


def smoothing_cpp(value: Value) -> str:
    values = numeric_list(value, {1, 4}, "smoothing", allow_percent=True)
    if len(values) == 1:
        return f"slugia::CornerSmoothing::all({values[0]})"
    return "slugia::CornerSmoothing{" + ", ".join(values) + "}"


def border_widths_cpp(value: Value) -> str:
    values = numeric_list(value, {4}, "stroke-width")
    return "slugia::BorderWidths{" + ", ".join(values) + "}"


def paint_cpp(value: Value) -> str:
    if value.kind == "color":
        return f"slugia::Paint::solid({color_cpp(value)})"
    if value.kind != "call":
        raise CompileError("expected color or paint function", value.token)
    name = str(value.value)
    args = value.arguments
    if name == "solid":
        if len(args) not in (1, 2):
            raise CompileError("solid(color[, opacity]) expects 1 or 2 arguments", value.token)
        result = f"slugia::Paint::solid({color_cpp(args[0])}"
        if len(args) == 2:
            result += f", {scalar_from_value(args[1])}"
        return result + ")"
    kinds = {
        "linear": "Linear",
        "diamond": "Diamond",
        "radial": "Radial",
    }
    if name in kinds:
        if len(args) not in (2, 6, 7):
            raise CompileError(
                f"{name}(start, end[, ox, oy, tx, ty[, opacity]]) expects 2, 6, or 7 arguments",
                value.token,
            )
        result = (
            f"slugia::Paint::gradient(slugia::GradientKind::{kinds[name]}, "
            f"{color_cpp(args[0])}, {color_cpp(args[1])}"
        )
        if len(args) >= 6:
            result += (
                f", {{{scalar_from_value(args[2])}, {scalar_from_value(args[3])}}}, "
                f"{{{scalar_from_value(args[4])}, {scalar_from_value(args[5])}}}"
            )
        if len(args) == 7:
            result += f", {scalar_from_value(args[6])}"
        return result + ")"
    if name == "shader":
        if len(args) not in (2, 3, 7, 8):
            raise CompileError(
                "shader(start, end[, parameter[, ox, oy, tx, ty[, opacity]]]) has invalid arguments",
                value.token,
            )
        parameter = scalar_from_value(args[2]) if len(args) >= 3 else "0.0f"
        result = f"slugia::Paint::shader({color_cpp(args[0])}, {color_cpp(args[1])}, {parameter}"
        if len(args) >= 7:
            result += (
                f", {{{scalar_from_value(args[3])}, {scalar_from_value(args[4])}}}, "
                f"{{{scalar_from_value(args[5])}, {scalar_from_value(args[6])}}}"
            )
        if len(args) == 8:
            result += f", {scalar_from_value(args[7])}"
        return result + ")"
    raise CompileError(f"unknown paint function '{name}'", value.token)


def literal_cpp(value: Value, expected: str) -> str:
    if expected == "bool":
        if value.kind != "bool":
            raise CompileError("expected boolean", value.token)
        return "true" if value.value else "false"
    if expected == "int":
        return number_text(value, integer=True)
    if expected == "float":
        return scalar_from_value(value)
    if expected == "string":
        if value.kind != "string":
            raise CompileError("expected string", value.token)
        return f"std::string{{{cpp_string(str(value.value))}}}"
    if expected == "color":
        return color_cpp(value)
    if expected == "paint":
        return paint_cpp(value)
    if expected == "length":
        return length_cpp(value)
    if expected == "radii":
        return radii_cpp(value)
    if expected == "smoothing":
        return smoothing_cpp(value)
    if expected == "border-widths":
        return border_widths_cpp(value)
    raise AssertionError(expected)


class Generator:
    KINDS = {"Absolute", "Row", "Column", "Stack", "Rectangle", "Shape", "Text", "Instance"}
    LAYOUT_MEMBERS = {
        "x": ("x", "length"),
        "y": ("y", "length"),
        "width": ("width", "length"),
        "height": ("height", "length"),
        "min-width": ("minWidth", "length"),
        "min-height": ("minHeight", "length"),
        "max-width": ("maxWidth", "length"),
        "max-height": ("maxHeight", "length"),
        "preferred-width": ("preferredWidth", "length"),
        "preferred-height": ("preferredHeight", "length"),
        "grow": ("grow", "float"),
        "spacing": ("spacing", "length"),
        "counter-spacing": ("counterSpacing", "length"),
    }
    PROPERTY_EXPECTED = {
        "bool": "bool",
        "int": "int",
        "float": "float",
        "string": "string",
        "color": "color",
        "paint": "paint",
        "length": "length",
        "radii": "radii",
        "smoothing": "smoothing",
        "border-widths": "border-widths",
    }

    def __init__(self, component: ComponentDecl, namespace: str,
                 class_name: str | None = None, legacy_figma_coordinates: bool = False,
                 strip_source_metadata: bool = False,
                 library_definitions: dict[str, ComponentDecl] | None = None,
                 definition_vars: dict[str, str] | None = None):
        self.component = component
        self.namespace = namespace
        self.class_name = class_name or cpp_identifier(component.name)
        self.legacy_figma_coordinates = legacy_figma_coordinates
        self.strip_source_metadata = strip_source_metadata
        self.properties = {prop.name: prop for prop in component.properties}
        self.property_members = {name: cpp_identifier(name) for name in self.properties}
        if len(set(self.property_members.values())) != len(self.property_members):
            raise CompileError("property names collide after C++ identifier normalization", component.token)
        own_definitions = {definition.name: definition for definition in component.definitions}
        self.definitions = library_definitions if library_definitions is not None else own_definitions
        if library_definitions is None and len(own_definitions) != len(component.definitions):
            raise CompileError("definition names must be unique", component.token)
        if component.name in self.definitions:
            raise CompileError(
                f"definition '{component.name}' conflicts with exported component name",
                component.token)
        self.definition_vars = dict(definition_vars or {})
        self.callbacks: dict[str, str] = {}
        self.node_counter = 0
        self.asset_counter = 0
        self.path_assets: dict[tuple[str, ...], tuple[str, Bounds]] = {}

    def _value(self, value: Value, expected: str) -> str:
        if value.kind == "reference":
            name = str(value.value)
            declaration = self.properties.get(name)
            if declaration is None:
                raise CompileError(f"unknown property '{name}'", value.token)
            actual = self.PROPERTY_EXPECTED[declaration.type_name]
            if actual != expected:
                raise CompileError(
                    f"property '{name}' is {declaration.type_name}, expected {expected}",
                    value.token,
                )
            return f"result.{self.property_members[name]}"
        return literal_cpp(value, expected)

    @staticmethod
    def _enum(value: Value, allowed: dict[str, str], label: str) -> str:
        if value.kind not in ("enum", "string"):
            raise CompileError(f"expected {label}", value.token)
        key = str(value.value).lower()
        if key not in allowed:
            raise CompileError(f"unknown {label} '{value.value}'", value.token)
        return allowed[key]

    @staticmethod
    def _plain_name(value: Value, label: str) -> str:
        if value.kind not in ("enum", "string"):
            raise CompileError(f"expected {label} name", value.token)
        return str(value.value)

    def _insets(self, value: Value) -> str:
        values = value.arguments if value.kind == "list" else [value]
        if len(values) not in (1, 2, 4):
            raise CompileError("padding expects 1, 2, or 4 lengths", value.token)
        resolved = [self._value(item, "length") for item in values]
        if len(resolved) == 1:
            return f"slugia::slugui::Insets::all({resolved[0]})"
        if len(resolved) == 2:
            return f"slugia::slugui::Insets::symmetric({resolved[1]}, {resolved[0]})"
        return "slugia::slugui::Insets{" + ", ".join(resolved) + "}"

    def _line(self, lines: list[str], indent: int, text: str) -> None:
        lines.append("  " * indent + text)

    def _assign_string(self, lines: list[str], indent: int, target: str, value: str) -> None:
        chunks = cpp_string_chunks(value)
        self._line(lines, indent, f"{target} = std::string{{{chunks[0]}}};")
        for chunk in chunks[1:]:
            self._line(lines, indent, f"{target}.append({chunk});")

    @staticmethod
    def _path_strings(value: Value, label: str) -> list[str]:
        values = value.arguments if value.kind == "list" else [value]
        if not values or any(item.kind != "string" for item in values):
            raise CompileError(f"{label} expects a path string or non-empty string list", value.token)
        return [str(item.value) for item in values]

    @staticmethod
    def _path_winding_rules(element: ElementDecl, attribute: Attribute,
                            count: int) -> tuple[str, ...]:
        companion_name = (
            "path-winding-rules" if attribute.name == "path-data"
            else "stroke-winding-rules"
        )
        companion = next(
            (candidate for candidate in element.attributes if candidate.name == companion_name),
            None,
        )
        if companion is None:
            return tuple("nonzero" for _ in range(count))
        values = companion.value.arguments if companion.value.kind == "list" else [companion.value]
        if len(values) != count:
            raise CompileError(
                f"{companion_name} must contain one rule per {attribute.name} entry",
                companion.token,
            )
        result: list[str] = []
        for value in values:
            if value.kind not in ("enum", "string"):
                raise CompileError(f"{companion_name} expects nonzero/evenodd names", value.token)
            rule = str(value.value).lower().replace("-", "").replace("_", "")
            if rule not in ("nonzero", "evenodd"):
                raise CompileError(f"unknown winding rule '{value.value}'", value.token)
            result.append(rule)
        return tuple(result)

    def _path_asset_key(self, element: ElementDecl,
                        attribute: Attribute) -> tuple[tuple[str, str], ...]:
        path_strings = tuple(self._path_strings(attribute.value, attribute.name))
        winding_rules = self._path_winding_rules(element, attribute, len(path_strings))
        return tuple(zip(path_strings, winding_rules))

    def _emit_path_asset(self, element: ElementDecl, attribute: Attribute,
                         lines: list[str], indent: int) -> tuple[str, Bounds]:
        key = self._path_asset_key(element, attribute)
        cached = self.path_assets.get(key)
        if cached is not None:
            return cached
        index = self.asset_counter
        self.asset_counter += 1
        asset = f"asset_{index}"
        path = f"asset_path_{index}"
        self._line(lines, indent, f"slugia::ShapeId {asset} = 0;")
        self._line(lines, indent, "if (atlas != nullptr) {")
        self._line(lines, indent + 1, f"slugia::Path {path};")
        all_operations = []
        winding_rules = {winding_rule for _, winding_rule in key}
        if len(winding_rules) > 1:
            raise CompileError(
                "mixed nonzero/evenodd rules in one Shape are not representable as one Slug "
                "shape; export them as separate vector layers", attribute.token)
        fill_rule = next(iter(winding_rules), "nonzero")
        for path_index, (data, winding_rule) in enumerate(key):
            try:
                operations = parse_svg_path(data)
            except SvgPathError as error:
                raise CompileError(f"invalid SVG path data: {error}", attribute.token) from error
            all_operations.extend(operations)
            subpath = f"{path}_part_{path_index}"
            data_variable = f"{subpath}_data"
            self._line(lines, indent + 1, f"slugia::Path {subpath};")
            self._line(lines, indent + 1, f"std::string {data_variable};")
            # Keep validated SVG compact in generated C++. Expanding every command into a separate
            # moveTo/cubicTo statement makes large Figma documents tens of megabytes larger and
            # turns MSVC parsing into the dominant import cost.
            self._assign_string(lines, indent + 1, data_variable, data)
            self._line(lines, indent + 1, f"{subpath}.svgPathYDown({data_variable});")
            self._line(lines, indent + 1, f"{path}.addPath({subpath});")
        bounds = operation_bounds(all_operations)
        if bounds is None or bounds.width <= 0.0 or bounds.height <= 0.0:
            raise CompileError("SVG path data has no two-dimensional drawable bounds",
                               attribute.token)
        rule = ("slugia::FillRule::EvenOdd" if fill_rule == "evenodd"
                else "slugia::FillRule::NonZero")
        self._line(lines, indent + 1, f"{asset} = atlas->addPath({path}, {rule});")
        self._line(lines, indent, "}")
        result = (asset, bounds)
        self.path_assets[key] = result
        return result

    def _prepare_path_assets(self, element: ElementDecl, lines: list[str], indent: int) -> None:
        for attribute in element.attributes:
            if attribute.name in ("path-data", "stroke-path-data"):
                self._emit_path_asset(element, attribute, lines, indent)
        for child in element.children:
            self._prepare_path_assets(child, lines, indent)

    def _path_asset(self, element: ElementDecl,
                    attribute: Attribute) -> tuple[str, Bounds]:
        key = self._path_asset_key(element, attribute)
        asset = self.path_assets.get(key)
        if asset is None:
            raise AssertionError("path assets must be prepared before node generation")
        return asset

    @staticmethod
    def _fixed_scalar(element: ElementDecl, name: str) -> float | None:
        attribute = next(
            (candidate for candidate in element.attributes if candidate.name == name), None,
        )
        if attribute is None:
            return None
        value = attribute.value
        raw: object
        if value.kind == "number":
            raw = value.value
        elif value.kind == "length" and value.value[1] in ("px", "ppx"):
            raw = value.value[0]
        else:
            return None
        try:
            extent = float(raw)
        except (TypeError, ValueError):
            return None
        return extent if math.isfinite(extent) else None

    @classmethod
    def _fixed_extent(cls, element: ElementDecl, name: str,
                      allow_zero: bool = False) -> float | None:
        extent = cls._fixed_scalar(element, name)
        if extent is None or extent < 0.0 or (extent == 0.0 and not allow_zero):
            return None
        return extent

    @classmethod
    def _authored_extent(cls, element: ElementDecl, name: str,
                         allow_zero: bool = False) -> float | None:
        # Figma FILL/HUG nodes intentionally omit width/height from layout, but vector
        # path-placement still needs the source node's authored extent for normalization.
        # preferred-{width,height} is the non-constraining source-size channel used by
        # current exporters; fixed width/height remains authoritative when present.
        extent = cls._fixed_extent(element, name, allow_zero)
        if extent is not None:
            return extent
        return cls._fixed_extent(element, "preferred-" + name, allow_zero)

    @staticmethod
    def _is_figma_element(element: ElementDecl) -> bool:
        provider = next(
            (candidate.value for candidate in element.attributes
             if candidate.name == "source-provider"), None)
        return (provider is not None and provider.kind == "string" and
                provider.value == "figma")

    @staticmethod
    def _placement_values(attribute: Attribute) -> list[float]:
        values = attribute.value.arguments if attribute.value.kind == "list" else []
        if len(values) != 4:
            raise CompileError(f"{attribute.name} expects [x, y, width, height]", attribute.token)
        parsed: list[float] = []
        for value in values:
            if value.kind == "number":
                raw = value.value
            elif value.kind == "length" and value.value[1] in ("px", "ppx"):
                raw = value.value[0]
            else:
                raise CompileError(f"{attribute.name} values must be px or numbers", value.token)
            try:
                number = float(raw)
            except (TypeError, ValueError) as error:
                raise CompileError(f"invalid {attribute.name} value", value.token) from error
            if not math.isfinite(number):
                raise CompileError(f"invalid {attribute.name} value", value.token)
            parsed.append(number)
        if parsed[2] <= 0.0 or parsed[3] <= 0.0:
            raise CompileError(f"{attribute.name} width and height must be positive", attribute.token)
        return parsed

    def _recovered_figma_owner(self, element: ElementDecl) -> tuple[float, float, float, float] | None:
        if not self._is_figma_element(element):
            return None
        width = self._authored_extent(element, "width", allow_zero=True)
        height = self._authored_extent(element, "height", allow_zero=True)
        if width is not None and height is not None and width > 0.0 and height > 0.0:
            return None
        placements = [
            self._placement_values(attribute) for attribute in element.attributes
            if attribute.name in ("path-placement", "stroke-placement")
        ]
        if not placements:
            return None
        left = min(value[0] for value in placements)
        top = min(value[1] for value in placements)
        right = max(value[0] + value[2] for value in placements)
        bottom = max(value[1] + value[3] for value in placements)
        # Older Figma exports omitted FILL/HUG extents entirely. Recover only the missing
        # axes from the authored geometry union; keep any explicit axis authoritative.
        origin_x = 0.0
        origin_y = 0.0
        if width is None or width <= 0.0:
            origin_x = left
            width = right - left
        if height is None or height <= 0.0:
            origin_y = top
            height = bottom - top
        if width <= 0.0 or height <= 0.0:
            return None
        return origin_x, origin_y, width, height

    def _path_placement(self, element: ElementDecl, bounds: Bounds) -> str | None:
        width = self._authored_extent(element, "width")
        height = self._authored_extent(element, "height")
        if width is None or height is None:
            return None
        values = (
            bounds.minimum_x / width,
            bounds.minimum_y / height,
            bounds.width / width,
            bounds.height / height,
        )
        return "slugia::Rect{" + ", ".join(
            cpp_float(f"{value:.9g}") for value in values
        ) + "}"

    def _explicit_path_placement(self, element: ElementDecl,
                                 attribute: Attribute | None) -> str | None:
        if attribute is None:
            return None
        parsed = self._placement_values(attribute)
        width = self._authored_extent(element, "width", allow_zero=True)
        height = self._authored_extent(element, "height", allow_zero=True)
        recovered_owner = self._recovered_figma_owner(element)
        if (width is None or height is None) and recovered_owner is None:
            raise CompileError(
                f"{attribute.name} requires authored width and height", attribute.token)
        if recovered_owner is not None:
            parsed[0] -= recovered_owner[0]
            parsed[1] -= recovered_owner[1]
            width = recovered_owner[2]
            height = recovered_owner[3]
        if width <= 0.0 or height <= 0.0:
            raise CompileError(f"{attribute.name} requires positive fixed width and height",
                               attribute.token)
        disjoint = (parsed[0] + parsed[2] < 0 or parsed[0] > width or
                    parsed[1] + parsed[3] < 0 or parsed[1] > height)
        if (self._is_figma_element(element) and recovered_owner is None and
                (self.legacy_figma_coordinates or disjoint)):
            parsed[0] = (width - parsed[2]) * 0.5
            parsed[1] = (height - parsed[3]) * 0.5
        normalized = (parsed[0] / width, parsed[1] / height,
                      parsed[2] / width, parsed[3] / height)
        return "slugia::Rect{" + ", ".join(
            cpp_float(f"{value:.9g}") for value in normalized
        ) + "}"

    def _common_attribute(self, variable: str, attribute: Attribute,
                          lines: list[str], indent: int) -> bool:
        name, value = attribute.name, attribute.value
        if name in self.LAYOUT_MEMBERS:
            member, expected = self.LAYOUT_MEMBERS[name]
            self._line(lines, indent,
                       f"{variable}.layout.{member} = {self._value(value, expected)};")
            return True
        padding_members = {
            "padding-left": "left",
            "padding-top": "top",
            "padding-right": "right",
            "padding-bottom": "bottom",
        }
        if name == "padding":
            self._line(lines, indent, f"{variable}.layout.padding = {self._insets(value)};")
            return True
        if name in padding_members:
            self._line(
                lines, indent,
                f"{variable}.layout.padding.{padding_members[name]} = {self._value(value, 'length')};",
            )
            return True
        if name == "layout":
            enum_value = self._enum(value, {
                "absolute": "Absolute", "row": "Row", "column": "Column", "stack": "Stack",
            }, "layout")
            self._line(lines, indent,
                       f"{variable}.layout.kind = slugia::slugui::LayoutKind::{enum_value};")
            return True
        if name == "position":
            positioned = self._enum(value, {
                "absolute": "true", "flow": "false", "auto": "false",
            }, "position")
            self._line(lines, indent,
                       f"{variable}.layout.absolutePositioned = {positioned};")
            return True
        if name in ("constraint-horizontal", "constraint-vertical"):
            enum_value = self._enum(value, {
                "min": "Min", "center": "Center", "max": "Max",
                "stretch": "Stretch", "scale": "Scale",
            }, "constraint")
            member = "horizontalConstraint" if name == "constraint-horizontal" else "verticalConstraint"
            self._line(lines, indent,
                       f"{variable}.layout.{member} = slugia::slugui::Constraint::{enum_value};")
            return True
        if name == "reverse-paint-order":
            self._line(lines, indent,
                       f"{variable}.layout.reverseChildPaintOrder = {self._value(value, 'bool')};")
            return True
        if name == "wrap":
            self._line(lines, indent,
                       f"{variable}.layout.wrap = {self._value(value, 'bool')};")
            return True
        if name in ("cross-align", "align-self"):
            enum_value = self._enum(value, {
                "start": "Start", "center": "Center", "end": "End", "stretch": "Stretch",
            }, "alignment")
            member = "crossAlignment" if name == "cross-align" else "alignSelf"
            self._line(lines, indent,
                       f"{variable}.layout.{member} = slugia::slugui::Alignment::{enum_value};")
            return True
        if name in ("justify", "counter-justify"):
            enum_value = self._enum(value, {
                "start": "Start", "center": "Center", "end": "End",
                "space-between": "SpaceBetween", "spacebetween": "SpaceBetween",
                "space-around": "SpaceAround", "spacearound": "SpaceAround",
                "space-evenly": "SpaceEvenly", "spaceevenly": "SpaceEvenly",
            }, "justification")
            member = "mainAlignment" if name == "justify" else "counterAlignment"
            self._line(lines, indent,
                       f"{variable}.layout.{member} = slugia::slugui::Justify::{enum_value};")
            return True
        if name == "visible":
            self._line(lines, indent, f"{variable}.visible = {self._value(value, 'bool')};")
            return True
        if name in ("clip", "overlay", "interactive"):
            resolved = self._value(value, "bool")
            member = {"clip": "clipsChildren", "overlay": "overlay",
                      "interactive": "interaction.interactive"}[name]
            self._line(lines, indent, f"{variable}.{member} = {resolved};")
            return True
        if name == "enabled":
            self._line(lines, indent,
                       f"{variable}.interaction.enabled = {self._value(value, 'bool')};")
            return True
        if name == "callback":
            callback = self._plain_name(value, "callback")
            member = "callback_" + cpp_identifier(callback)
            previous = next((key for key, item in self.callbacks.items() if item == member), None)
            if previous is not None and previous != callback:
                raise CompileError("callback names collide after C++ normalization", value.token)
            self.callbacks[callback] = member
            self._line(lines, indent, f"{variable}.interaction.interactive = true;")
            self._line(lines, indent, f"{variable}.interaction.callback = {member};")
            return True
        source_members = {
            "source-provider": "provider",
            "source-document": "documentId",
            "source-node": "nodeId",
            "source-name": "nodeName",
            "source-provider-data": "providerData",
        }
        if name in source_members:
            if value.kind != "string":
                raise CompileError(f"{name} expects a string", value.token)
            if self.strip_source_metadata:
                return True
            string_value = str(value.value)
            if name == "source-provider-data":
                string_value = compact_figma_provider_data(string_value)
            self._assign_string(
                lines, indent, f"{variable}.source.{source_members[name]}", string_value)
            return True
        if name == "fidelity":
            enum_value = self._enum(value, {
                "native": "Native", "approximated": "Approximated",
                "baked-vector": "BakedVector", "bakedvector": "BakedVector",
                "baked-raster": "BakedRaster", "bakedraster": "BakedRaster",
                "unsupported": "Unsupported",
            }, "fidelity")
            if not self.strip_source_metadata:
                self._line(lines, indent,
                           f"{variable}.source.fidelity = slugia::slugui::ImportFidelity::{enum_value};")
            return True
        return False

    def _emit_text_runs(self, element: ElementDecl, variable: str,
                        lines: list[str], indent: int) -> None:
        attribute = next(
            (candidate for candidate in element.attributes if candidate.name == "text-runs"), None)
        if attribute is None:
            return
        values = attribute.value.arguments if attribute.value.kind == "list" else []
        if not values:
            raise CompileError("text-runs expects a non-empty list", attribute.token)
        text_attribute = next(
            (candidate for candidate in element.attributes if candidate.name == "text"), None)
        if text_attribute is None or text_attribute.value.kind != "string":
            raise CompileError("text-runs requires literal text", attribute.token)
        concatenated = ""
        visual = f"std::get<slugia::slugui::TextVisual>({variable}.visual)"
        for index, value in enumerate(values):
            if value.kind != "call" or value.value != "text-run" or len(value.arguments) not in (10, 11):
                raise CompileError(
                    "each text-runs item must be text-run(text, font, size, weight, italic, "
                    "underline, strikethrough, letter-spacing, line-height, paint[, font-style])",
                    value.token)
            (text, font, size, weight, italic, underline, strikethrough,
             letter_spacing, line_height, paint) = value.arguments[:10]
            font_style = value.arguments[10] if len(value.arguments) == 11 else None
            if text.kind != "string" or font.kind != "string":
                raise CompileError("text-run text and font must be strings", value.token)
            if weight.kind != "number":
                raise CompileError("text-run weight must be a number", weight.token)
            parsed_weight = float(str(weight.value))
            if not math.isfinite(parsed_weight) or parsed_weight < 1 or parsed_weight > 1000:
                raise CompileError("text-run weight must be between 1 and 1000", weight.token)
            concatenated += str(text.value)
            style = f"text_run_style_{variable}_{index}"
            self._line(lines, indent, "{")
            self._line(lines, indent + 1, f"auto {style} = {visual}.style;")
            self._line(lines, indent + 1,
                       f"{style}.fontName = {cpp_string(str(font.value))};")
            if font_style is not None:
                if font_style.kind != "string":
                    raise CompileError("text-run font-style must be a string", font_style.token)
                self._line(lines, indent + 1,
                           f"{style}.fontStyle = {cpp_string(str(font_style.value))};")
            self._line(lines, indent + 1,
                       f"{style}.size = {literal_cpp(size, 'float')};")
            self._line(lines, indent + 1,
                       f"{style}.weight = {int(round(parsed_weight))};")
            self._line(lines, indent + 1,
                       f"{style}.italic = {literal_cpp(italic, 'bool')};")
            self._line(lines, indent + 1,
                       f"{style}.underline = {literal_cpp(underline, 'bool')};")
            self._line(lines, indent + 1,
                       f"{style}.strikethrough = {literal_cpp(strikethrough, 'bool')};")
            self._line(lines, indent + 1,
                       f"{style}.letterSpacing = {literal_cpp(letter_spacing, 'float')};")
            self._line(lines, indent + 1,
                       f"{style}.lineHeight = {literal_cpp(line_height, 'float')};")
            self._line(lines, indent + 1,
                       f"{style}.paint = {paint_cpp(paint)};")
            self._line(
                lines, indent + 1,
                f"{visual}.runs.push_back(slugia::TextRun{{"
                f"{cpp_string(str(text.value))}, std::move({style})}});")
            self._line(lines, indent, "}")
        if concatenated != str(text_attribute.value.value):
            raise CompileError("text-runs characters must exactly match text", attribute.token)

    def _visual_attribute(self, element: ElementDecl, variable: str, attribute: Attribute,
                          lines: list[str], indent: int) -> bool:
        name, value = attribute.name, attribute.value
        visual = f"std::get<slugia::slugui::{element.kind if element.kind != 'Rectangle' else 'RoundedRectangle'}Visual>({variable}.visual)"
        if name in ("fill", "fill-hovered", "fill-pressed", "fill-disabled"):
            if element.kind not in ("Rectangle", "Shape", "Text"):
                raise CompileError(f"{name} requires a visual element", attribute.token)
            state = {
                "fill": "normal", "fill-hovered": "hovered",
                "fill-pressed": "pressed", "fill-disabled": "disabled",
            }[name]
            resolved = self._value(value, "paint")
            if state == "normal":
                self._line(lines, indent, f"{visual}.paint.normal = {resolved};")
            else:
                self._line(
                    lines, indent,
                    f"{visual}.paint.{state} = slugia::slugui::ValueSource<slugia::Paint>{{{resolved}}};",
                )
            return True
        if name in ("stroke", "stroke-hovered", "stroke-pressed", "stroke-disabled"):
            if element.kind not in ("Rectangle", "Shape"):
                raise CompileError(f"{name} requires Rectangle or Shape", attribute.token)
            state = {
                "stroke": "normal", "stroke-hovered": "hovered",
                "stroke-pressed": "pressed", "stroke-disabled": "disabled",
            }[name]
            resolved = self._value(value, "paint")
            target = f"{visual}.stroke.paint" if element.kind == "Rectangle" else (
                f"{visual}.strokePaint"
            )
            if state == "normal":
                self._line(lines, indent, f"{target}.normal = {resolved};")
            else:
                self._line(
                    lines, indent,
                    f"{target}.{state} = slugia::slugui::ValueSource<slugia::Paint>{{{resolved}}};",
                )
            return True
        if name == "stroke-width":
            if element.kind == "Rectangle":
                border_width_reference = value.kind == "reference" and (
                    self.properties.get(str(value.value)) is not None and
                    self.properties[str(value.value)].type_name == "border-widths"
                )
                if value.kind == "list" or border_width_reference:
                    self._line(lines, indent,
                               f"{visual}.stroke.individualWidths = "
                               f"slugia::slugui::ValueSource<slugia::BorderWidths>{{"
                               f"{self._value(value, 'border-widths')}}};")
                else:
                    self._line(lines, indent,
                               f"{visual}.stroke.width = {self._value(value, 'float')};")
            elif element.kind == "Shape":
                if value.kind == "list":
                    raise CompileError("Shape stroke-width must be uniform", attribute.token)
                self._value(value, 'float')
            else:
                raise CompileError("stroke-width requires Rectangle or Shape", attribute.token)
            return True
        if name == "stroke-align":
            if element.kind not in ("Rectangle", "Shape"):
                raise CompileError("stroke-align requires Rectangle or Shape", attribute.token)
            enum_value = self._enum(value, {
                "inside": "Inside", "center": "Center", "outside": "Outside",
            }, "stroke alignment")
            if element.kind == "Rectangle":
                self._line(lines, indent,
                           f"{visual}.stroke.align = slugia::StrokeAlign::{enum_value};")
            return True
        if name in ("path-data", "stroke-path-data"):
            if element.kind != "Shape":
                raise CompileError(f"{name} requires Shape", attribute.token)
            self._path_strings(value, name)
            return True
        if name in ("path-winding-rules", "stroke-winding-rules"):
            if element.kind != "Shape":
                raise CompileError(f"{name} requires Shape", attribute.token)
            # Parsed together with path-data/stroke-path-data when the atlas asset is prepared.
            return True
        if name in ("path-placement", "stroke-placement"):
            if element.kind != "Shape":
                raise CompileError(f"{name} requires Shape", attribute.token)
            return True
        if name in ("image-source-width", "image-source-height"):
            if element.kind != "Rectangle":
                raise CompileError(f"{name} requires Rectangle", attribute.token)
            if value.kind != "number":
                raise CompileError(f"{name} expects a number", value.token)
            numeric = float(str(value.value))
            if not math.isfinite(numeric) or numeric < 0.0:
                raise CompileError(f"{name} must be finite and non-negative", value.token)
            member = "sourceWidth" if name == "image-source-width" else "sourceHeight"
            self._line(lines, indent, f"{visual}.image.enabled = true;")
            self._line(lines, indent,
                       f"{visual}.image.{member} = {cpp_float(f'{numeric:.9g}')};")
            return True
        if name == "image-scale-mode":
            if element.kind != "Rectangle":
                raise CompileError("image-scale-mode requires Rectangle", attribute.token)
            enum_value = self._enum(value, {
                "fill": "Fill", "fit": "Fit", "crop": "Crop", "tile": "Tile",
            }, "image scale mode")
            self._line(lines, indent, f"{visual}.image.enabled = true;")
            self._line(lines, indent,
                       f"{visual}.image.scaleMode = slugia::slugui::ImageScaleMode::{enum_value};")
            return True
        if name == "radius":
            if element.kind != "Rectangle":
                raise CompileError("radius requires Rectangle", attribute.token)
            self._line(lines, indent, f"{visual}.radii = {self._value(value, 'radii')};")
            return True
        if name == "smoothing":
            if element.kind != "Rectangle":
                raise CompileError("smoothing requires Rectangle", attribute.token)
            self._line(lines, indent,
                       f"{visual}.smoothing = {self._value(value, 'smoothing')};")
            return True
        if name == "shape":
            if element.kind != "Shape":
                raise CompileError("shape id requires Shape", attribute.token)
            self._line(lines, indent, f"{visual}.shape = {literal_cpp(value, 'int')};")
            return True
        if name == "text":
            if element.kind != "Text":
                raise CompileError("text requires Text", attribute.token)
            self._line(lines, indent, f"{visual}.text = {self._value(value, 'string')};")
            return True
        if name == "text-runs":
            if element.kind != "Text":
                raise CompileError("text-runs requires Text", attribute.token)
            return True
        if element.kind != "Text":
            return False
        style = f"{visual}.style"
        if name == "font":
            if value.kind != "string":
                raise CompileError("font expects a string", value.token)
            self._line(lines, indent, f"{style}.fontName = {cpp_string(str(value.value))};")
            return True
        if name == "font-style":
            if value.kind != "string":
                raise CompileError("font-style expects a string", value.token)
            self._line(lines, indent, f"{style}.fontStyle = {cpp_string(str(value.value))};")
            return True
        if name == "font-weight":
            if value.kind != "number":
                raise CompileError("font-weight expects a number", value.token)
            weight = float(str(value.value))
            if not math.isfinite(weight) or weight < 1 or weight > 1000:
                raise CompileError("font-weight must be between 1 and 1000", value.token)
            self._line(lines, indent, f"{style}.weight = {int(round(weight))};")
            return True
        float_fields = {
            "font-size": "size", "line-height": "lineHeight",
            "letter-spacing": "letterSpacing", "indent": "indent",
        }
        if name in float_fields:
            self._line(lines, indent,
                       f"{style}.{float_fields[name]} = {literal_cpp(value, 'float')};")
            return True
        bool_fields = {
            "bold": "bold", "italic": "italic", "underline": "underline",
            "strikethrough": "strikethrough",
        }
        if name in bool_fields:
            self._line(lines, indent,
                       f"{style}.{bool_fields[name]} = {literal_cpp(value, 'bool')};")
            return True
        if name == "text-align":
            enum_value = self._enum(value, {
                "left": "Left", "center": "Center", "right": "Right", "justify": "Justify",
                "justified": "Justify",
            }, "text alignment")
            self._line(lines, indent,
                       f"{style}.align = slugia::HorizontalAlign::{enum_value};")
            return True
        if name == "text-align-vertical":
            enum_value = self._enum(value, {
                "top": "Top", "center": "Center", "bottom": "Bottom",
            }, "vertical text alignment")
            self._line(lines, indent,
                       f"{style}.verticalAlign = slugia::VerticalAlign::{enum_value};")
            return True
        if name == "list-marker":
            enum_value = self._enum(value, {
                "none": "None", "bullet": "Bullet", "numbered": "Numbered",
            }, "list marker")
            self._line(lines, indent,
                       f"{style}.listMarker = slugia::ListMarker::{enum_value};")
            return True
        return False

    def _node(self, element: ElementDecl, path: str, lines: list[str], indent: int) -> str:
        if element.kind not in self.KINDS:
            raise CompileError(f"unknown element kind '{element.kind}'", element.token)
        variable = f"node_{self.node_counter}"
        self.node_counter += 1
        attributes = {attribute.name: attribute for attribute in element.attributes}
        provider = attributes.get("source-provider")
        document = attributes.get("source-document")
        source_node = attributes.get("source-node")
        figma_metadata: dict[str, object] = {}
        provider_data_attribute = attributes.get("source-provider-data")
        if (provider is not None and provider.value.kind == "string" and
                provider.value.value == "figma" and
                provider_data_attribute is not None and
                provider_data_attribute.value.kind == "string"):
            try:
                parsed_metadata = json.loads(str(provider_data_attribute.value.value))
                if isinstance(parsed_metadata, dict):
                    figma_metadata = parsed_metadata
            except (TypeError, ValueError, json.JSONDecodeError):
                pass
        if (provider is not None and provider.value.kind == "string" and
                provider.value.value == "figma" and
                document is not None and document.value.kind == "string" and
                source_node is not None and source_node.value.kind == "string" and
                str(source_node.value.value)):
            # Figma node IDs are the synchronization identity. Names and group nesting are
            # presentation details and may change during design edits without resetting UI state.
            stable_id = (
                f"figma/{document.value.value}/{source_node.value.value}"
            )
        else:
            stable_id = f"{self.component.name}/"
            if path:
                stable_id += path + "/"
            stable_id += element.name
        path_attribute = attributes.get("path-data")
        stroke_path_attribute = attributes.get("stroke-path-data")
        fill_placement_attribute = attributes.get("path-placement")
        stroke_placement_attribute = attributes.get("stroke-placement")
        shape_attribute = attributes.get("shape")
        if element.kind != "Shape" and (path_attribute or stroke_path_attribute):
            offending = path_attribute or stroke_path_attribute
            raise CompileError(f"{offending.name} requires Shape", offending.token)
        if path_attribute and shape_attribute:
            raise CompileError("Shape cannot specify both shape and path-data", shape_attribute.token)
        fill_asset, fill_bounds = (
            self._path_asset(element, path_attribute)
            if path_attribute else ("0", None)
        )
        stroke_asset, stroke_bounds = (
            self._path_asset(element, stroke_path_attribute)
            if stroke_path_attribute else ("0", None)
        )
        arguments = f"slugia::hashId({cpp_string(stable_id)}), {cpp_string(element.name)}"
        instance_definition: ComponentDecl | None = None
        instance_definition_var = ""
        instance_special_attributes = {
            "definition", "instance-id", "property-links", "property-overrides",
        }
        if element.kind == "Instance":
            if element.children:
                raise CompileError("Instance cannot contain authored child elements", element.token)
            definition_attribute = attributes.get("definition")
            instance_id_attribute = attributes.get("instance-id")
            if definition_attribute is None:
                raise CompileError("Instance requires definition", element.token)
            if instance_id_attribute is None:
                raise CompileError("Instance requires instance-id", element.token)
            if definition_attribute.value.kind not in ("enum", "string"):
                raise CompileError("Instance definition expects a definition name", definition_attribute.token)
            definition_name = str(definition_attribute.value.value)
            instance_definition = self.definitions.get(definition_name)
            if instance_definition is None:
                raise CompileError(f"unknown component definition '{definition_name}'",
                                   definition_attribute.token)
            instance_definition_var = self.definition_vars.get(definition_name, "")
            if not instance_definition_var:
                raise CompileError(
                    f"component definition '{definition_name}' was not prepared",
                    definition_attribute.token)
            if instance_id_attribute.value.kind == "string":
                local_id = f"slugia::hashId({cpp_string(str(instance_id_attribute.value.value))})"
            elif instance_id_attribute.value.kind == "number":
                local_id_text = number_text(instance_id_attribute.value, integer=True)
                if int(local_id_text) == 0:
                    raise CompileError("Instance instance-id must be non-zero",
                                       instance_id_attribute.token)
                local_id = f"static_cast<slugia::slugui::InstanceId>({local_id_text})"
            else:
                raise CompileError("Instance instance-id expects a string or integer",
                                   instance_id_attribute.token)
            factory = (
                f"slugia::slugui::instance(slugia::hashId({cpp_string(stable_id)}), "
                f"{local_id}, {instance_definition_var}.definition, {cpp_string(element.name)})"
            )
        else:
            factories = {
                "Absolute": f"slugia::slugui::absolute({arguments})",
                "Row": f"slugia::slugui::row({arguments})",
                "Column": f"slugia::slugui::column({arguments})",
                "Stack": f"slugia::slugui::stack({arguments})",
                "Rectangle": (
                    f"slugia::slugui::roundedRectangle(slugia::hashId({cpp_string(stable_id)}), "
                    f"slugia::Paint::solid(slugia::Color{{}}), slugia::CornerRadii{{}}, "
                    f"slugia::CornerSmoothing{{}}, {cpp_string(element.name)})"
                ),
                "Shape": (
                    f"slugia::slugui::shape(slugia::hashId({cpp_string(stable_id)}), {fill_asset}, "
                    f"slugia::Paint::solid(slugia::Color{{}}), {cpp_string(element.name)})"
                ),
                "Text": (
                    f"slugia::slugui::text(slugia::hashId({cpp_string(stable_id)}), "
                    f"std::string{{}}, slugia::TextStyle{{}}, {cpp_string(element.name)})"
                ),
            }
            factory = factories[element.kind]
        self._line(lines, indent, f"auto {variable} = {factory};")
        shape_visual = f"std::get<slugia::slugui::ShapeVisual>({variable}.visual)"
        if fill_bounds is not None:
            placement = self._explicit_path_placement(element, fill_placement_attribute)
            if placement is None:
                placement = self._path_placement(element, fill_bounds)
            if placement is not None:
                self._line(lines, indent, f"{shape_visual}.fillPlacement = {placement};")
        if stroke_path_attribute:
            self._line(
                lines, indent,
                f"{shape_visual}.strokeShape = {stroke_asset};",
            )
            placement = self._explicit_path_placement(element, stroke_placement_attribute)
            if placement is None:
                placement = self._path_placement(element, stroke_bounds)
            if placement is not None:
                self._line(lines, indent, f"{shape_visual}.strokePlacement = {placement};")
        for attribute in element.attributes:
            if self._common_attribute(variable, attribute, lines, indent):
                continue
            if self._visual_attribute(element, variable, attribute, lines, indent):
                continue
            raise CompileError(
                f"attribute '{attribute.name}' is not valid on {element.kind}", attribute.token,
            )

        # Mirror key Figma semantics into typed metadata for normal generated components.
        # Preview/runtime-only codegen may strip them because layout/visual compatibility has
        # already been lowered into typed fields and the original .slugui remains lossless.
        if figma_metadata and not self.strip_source_metadata:
            string_fields = {
                "nodeType": "nodeType",
                "parentNodeId": "parentNodeId",
                "componentKey": "componentKey",
                "componentSetId": "componentSetId",
                "mainComponentId": "mainComponentId",
            }
            for source_key, member in string_fields.items():
                value = figma_metadata.get(source_key)
                if isinstance(value, str) and value:
                    self._assign_string(lines, indent, f"{variable}.source.{member}", value)

            node_type = str(figma_metadata.get("nodeType", "")).upper()
            figma_kinds = {
                "COMPONENT_SET": "ComponentSet",
                "COMPONENT": "Component",
                "INSTANCE": "Instance",
                "SLOT": "Slot",
            }
            if node_type in figma_kinds:
                self._line(
                    lines, indent,
                    f"{variable}.source.figmaKind = "
                    f"slugia::slugui::FigmaSemanticKind::{figma_kinds[node_type]};")

            json_fields = {
                "variantProperties": "variantPropertiesJson",
                "variantSelection": "variantSelectionJson",
                "componentProperties": "componentPropertiesJson",
                "componentDefinitions": "componentDefinitionsJson",
                "componentPropertyReferences": "componentPropertyReferencesJson",
                "overrides": "overridesJson",
                "exposedInstanceIds": "exposedInstanceIdsJson",
                "imageFills": "imageFillsJson",
            }
            for source_key, member in json_fields.items():
                value = figma_metadata.get(source_key)
                if value is not None:
                    encoded = json.dumps(
                        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
                    self._assign_string(lines, indent, f"{variable}.source.{member}", encoded)

            scale_factor = figma_metadata.get("scaleFactor")
            if isinstance(scale_factor, (int, float)) and math.isfinite(float(scale_factor)):
                self._line(lines, indent,
                    f"{variable}.source.instanceScaleFactor = {cpp_float(f'{float(scale_factor):.9g}')};")
            exposed = figma_metadata.get("isExposedInstance")
            if isinstance(exposed, bool):
                self._line(lines, indent,
                    f"{variable}.source.exposedInstance = {'true' if exposed else 'false'};")

        # Legacy exporter compatibility: older Figma .slugui files stored constraints only in
        # source-provider-data. Recover them so existing dropped files get the same anchor
        # semantics as new exports with explicit constraint-horizontal/vertical attributes.
        if "constraint-horizontal" not in attributes or "constraint-vertical" not in attributes:
            constraints = figma_metadata.get("constraints") if figma_metadata else None
            mapping = {
                "MIN": "Min", "CENTER": "Center", "MAX": "Max",
                "STRETCH": "Stretch", "SCALE": "Scale",
            }
            if isinstance(constraints, dict):
                if "constraint-horizontal" not in attributes:
                    value = mapping.get(str(constraints.get("horizontal", "")).upper())
                    if value:
                        self._line(lines, indent,
                            f"{variable}.layout.horizontalConstraint = "
                            f"slugia::slugui::Constraint::{value};")
                if "constraint-vertical" not in attributes:
                    value = mapping.get(str(constraints.get("vertical", "")).upper())
                    if value:
                        self._line(lines, indent,
                            f"{variable}.layout.verticalConstraint = "
                            f"slugia::slugui::Constraint::{value};")

        recovered_owner = self._recovered_figma_owner(element)
        if element.kind == "Shape" and recovered_owner is not None:
            base_x = self._fixed_scalar(element, "x") or 0.0
            base_y = self._fixed_scalar(element, "y") or 0.0
            recovered = (
                base_x + recovered_owner[0], base_y + recovered_owner[1],
                recovered_owner[2], recovered_owner[3],
            )
            for member, value in zip(("x", "y", "width", "height"), recovered):
                self._line(
                    lines, indent,
                    f"{variable}.layout.{member} = slugia::slugui::Length::logical("
                    f"{cpp_float(f'{value:.9g}')});",
                )
        if element.kind == "Text":
            self._emit_text_runs(element, variable, lines, indent)
        child_path = f"{path}/{element.name}" if path else element.name
        for child in element.children:
            # Put every child subtree in a non-inlined lambda. MSVC /Od reserves stack and unwind
            # state for lexical locals until the containing function returns even after braces.
            # A separate callable bounds each generated node's temporaries to its own stack frame,
            # keeping large Figma component sets fast to compile without a giant runtime stack.
            child_index = self.node_counter
            built_variable = f"built_node_{child_index}"
            self._line(
                lines, indent,
                f"auto {built_variable} = [&]() -> slugia::slugui::Element {{",
            )
            child_variable = self._node(child, child_path, lines, indent + 1)
            self._line(lines, indent + 1, f"return {child_variable};")
            self._line(lines, indent, "}();")
            self._line(lines, indent, f"{variable}.add(std::move({built_variable}));")
        return variable

    def _font_requests(
            self) -> dict[tuple[str, int, bool, str], tuple[set[int], set[str]]]:
        requests: dict[tuple[str, int, bool, str], tuple[set[int], set[str]]] = {}

        def add(family: str, weight: int, italic: bool, text: str,
                font_style: str = "") -> None:
            if not family or family == "system-ui":
                return
            key = (family, max(1, min(1000, weight)), italic, font_style)
            points, samples = requests.setdefault(key, (set(), set()))
            points.update(ord(character) for character in text if character)
            if text:
                samples.add(text)

        def walk(element: ElementDecl) -> None:
            attributes = {attribute.name: attribute.value for attribute in element.attributes}
            if element.kind == "Text":
                font = attributes.get("font")
                family = str(font.value) if font and font.kind == "string" else "system-ui"
                style_value = attributes.get("font-style")
                font_style = str(style_value.value) if style_value and style_value.kind == "string" else ""
                weight_value = attributes.get("font-weight")
                weight = 400
                if weight_value and weight_value.kind == "number":
                    try:
                        weight = int(round(float(str(weight_value.value))))
                    except ValueError:
                        weight = 400
                italic_value = attributes.get("italic")
                italic = bool(italic_value.value) if italic_value and italic_value.kind == "bool" else False
                text_value = attributes.get("text")
                if text_value and text_value.kind == "string":
                    add(family, weight, italic, str(text_value.value), font_style)

                run_value = attributes.get("text-runs")
                if run_value and run_value.kind == "list":
                    for run in run_value.arguments:
                        if run.kind != "call" or str(run.value) != "text-run" or len(run.arguments) < 5:
                            continue
                        run_text, run_font, _size, run_weight, run_italic = run.arguments[:5]
                        if run_text.kind != "string" or run_font.kind != "string":
                            continue
                        parsed_weight = 400
                        if run_weight.kind == "number":
                            try:
                                parsed_weight = int(round(float(str(run_weight.value))))
                            except ValueError:
                                pass
                        parsed_italic = bool(run_italic.value) if run_italic.kind == "bool" else False
                        run_style = ""
                        if len(run.arguments) >= 11 and run.arguments[10].kind == "string":
                            run_style = str(run.arguments[10].value)
                        add(str(run_font.value), parsed_weight, parsed_italic,
                            str(run_text.value), run_style)
            for child in element.children:
                walk(child)

        walk(self.component.root)
        return requests

    def generate(self, source_name: str) -> str:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", self.component.name):
            raise CompileError("component name must be a C++ identifier", self.component.token)
        namespace_parts = self.namespace.split("::") if self.namespace else []
        if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", part) for part in namespace_parts):
            raise CompileError("namespace must contain C++ identifiers separated by ::")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", self.class_name):
            raise CompileError("generated class name must be a C++ identifier")

        build_lines: list[str] = []
        self._line(build_lines, 2, "(void)atlas;")
        font_sample_index = 0
        for (family, weight, italic, font_style), (codepoints, samples) in sorted(
                self._font_requests().items()):
            if not codepoints:
                continue
            points = ", ".join(f"{codepoint}u" for codepoint in sorted(codepoints))
            self._line(
                build_lines, 2,
                f"if (atlas) (void)atlas->loadSystemFont({cpp_string(family)}, {weight}, "
                f"{'true' if italic else 'false'}, std::vector<std::uint32_t>{{{points}}}, "
                f"{cpp_string(font_style)});")
            for sample in sorted(samples):
                variable = f"font_sample_{font_sample_index}"
                font_sample_index += 1
                self._line(build_lines, 2, f"std::string {variable};")
                self._assign_string(build_lines, 2, variable, sample)
                self._line(
                    build_lines, 2,
                    f"if (atlas) (void)atlas->prepareText({variable}, {cpp_string(family)}, "
                    f"{weight}, {'true' if italic else 'false'}, {cpp_string(font_style)});")
        self._line(build_lines, 2, "Built result;")
        for prop in self.component.properties:
            if prop.initial.kind == "reference":
                raise CompileError("property initializers cannot reference another property", prop.initial.token)
            expected = self.PROPERTY_EXPECTED[prop.type_name]
            initial = literal_cpp(prop.initial, expected)
            member = self.property_members[prop.name]
            self._line(
                build_lines, 2,
                f"result.{member} = result.properties.define<{PROPERTY_CPP[prop.type_name]}>("
                f"{cpp_string(prop.name)}, {initial});",
            )

        # Intern identical Figma vector geometry once per generated component. Component sets
        # commonly repeat the same icons in every variant; sharing the atlas asset cuts Python
        # parse work, generated C++ size, C++ compile time, and runtime atlas memory together.
        self._prepare_path_assets(self.component.root, build_lines, 2)
        root_variable = self._node(self.component.root, "", build_lines, 2)
        self._line(build_lines, 2, f"result.root = std::move({root_variable});")
        self._line(build_lines, 2, "return result;")

        class_name = self.class_name
        lines = [
            "#pragma once",
            "",
            '#include "slugia/slugui.hpp"',
            '#include "slugia/vector_atlas.hpp"',
            "",
            "#include <cstdint>",
            "#include <memory>",
            "#include <string>",
            "#include <utility>",
            "",
            f"// Generated by slugui_compiler.py from "
            f"{source_name.replace(chr(13), ' ').replace(chr(10), ' ')}. Do not edit.",
        ]
        for part in namespace_parts:
            lines.append(f"namespace {part} {{")
        if namespace_parts:
            lines.append("")
        lines.extend([
            f"struct {class_name} {{",
            "public:",
        ])
        for callback, member in sorted(self.callbacks.items()):
            lines.append(
                f"  static inline const slugia::slugui::CallbackId {member} = "
                f"slugia::hashId({cpp_string(self.component.name + '.callback.' + callback)});"
            )
        if self.callbacks:
            lines.append("")
        lines.extend([
            "  struct SharedDefinition {",
            "    std::shared_ptr<const slugia::slugui::ComponentDefinition> definition;",
        ])
        for prop in self.component.properties:
            lines.append(
                f"    slugia::slugui::Property<{PROPERTY_CPP[prop.type_name]}> "
                f"{self.property_members[prop.name]};"
            )
        lines.extend([
            "",
            "    [[nodiscard]] slugia::slugui::ComponentInstance instantiate(",
            "        slugia::slugui::InstanceId instance_id = 0) const {",
            "      return slugia::slugui::ComponentInstance(definition, instance_id);",
            "    }",
            "  };",
            "",
            "  [[nodiscard]] static SharedDefinition makeSharedDefinition() {",
            "    return makeSharedDefinition(nullptr);",
            "  }",
            "  [[nodiscard]] static SharedDefinition makeSharedDefinition(slugia::VectorAtlas& atlas) {",
            "    return makeSharedDefinition(&atlas);",
            "  }",
            "",
            "  slugia::slugui::Component component;",
        ])
        for prop in self.component.properties:
            lines.append(
                f"  slugia::slugui::Property<{PROPERTY_CPP[prop.type_name]}> "
                f"{self.property_members[prop.name]};"
            )
        lines.extend([
            "",
            f"  {class_name}() : {class_name}(build(nullptr)) {{}}",
            f"  explicit {class_name}(slugia::VectorAtlas& atlas) : {class_name}(build(&atlas)) {{}}",
            "",
            "private:",
            "  struct Built {",
            "    slugia::slugui::PropertyStore properties;",
            "    slugia::slugui::Element root;",
        ])
        for prop in self.component.properties:
            lines.append(
                f"    slugia::slugui::Property<{PROPERTY_CPP[prop.type_name]}> "
                f"{self.property_members[prop.name]};"
            )
        lines.extend([
            "  };",
            "",
            "  static Built build(slugia::VectorAtlas* atlas) {",
            *build_lines,
            "  }",
            "",
            "  static SharedDefinition makeSharedDefinition(slugia::VectorAtlas* atlas) {",
            "    auto built = build(atlas);",
            "    SharedDefinition result;",
        ])
        for prop in self.component.properties:
            member = self.property_members[prop.name]
            lines.append(f"    result.{member} = built.{member};")
        lines.extend([
            "    result.definition = std::make_shared<const slugia::slugui::ComponentDefinition>(",
            "      std::move(built.root), std::move(built.properties));",
            "    return result;",
            "  }",
            "",
            f"  explicit {class_name}(Built built)",
            "      : component(std::move(built.root), std::move(built.properties)),",
        ])
        if self.component.properties:
            for index, prop in enumerate(self.component.properties):
                suffix = "," if index + 1 < len(self.component.properties) else " {}"
                member = self.property_members[prop.name]
                lines.append(f"        {member}(built.{member}){suffix}")
        else:
            lines[-1] = "      : component(std::move(built.root), std::move(built.properties)) {}"
        lines.extend([
            "};",
            "",
        ])
        for part in reversed(namespace_parts):
            lines.append(f"}} // namespace {part}")
        return "\n".join(lines) + "\n"


def compile_text(source: str, source_name: str, namespace: str,
                 class_name: str | None = None,
                 strip_source_metadata: bool = False) -> str:
    component = parse_source(source)
    header = re.search(r"(?m)^\s*//\s*SlugUI exchange format\s+(\d+)\s*,", source)
    legacy_figma_coordinates = header is not None and int(header.group(1)) == 1
    return Generator(
        component, namespace, class_name, legacy_figma_coordinates,
        strip_source_metadata).generate(source_name)


def write_atomic(path: Path, content: str) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = content.encode("utf-8")
    try:
        # Keeping the timestamp stable is important: CMake/MSBuild then skips recompiling every
        # translation unit that includes an unchanged generated SlugUI header.
        if path.read_bytes() == encoded:
            return False
    except (FileNotFoundError, PermissionError):
        pass

    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(encoded)
        # A concurrent MSVC compile can keep an included generated header open without FILE_SHARE_DELETE
        # for several seconds. Wait for that reader rather than failing the import or overwriting the
        # file in place (which could give the compiler a partially-written header).
        for attempt in range(10):
            try:
                os.replace(temporary, path)
                return True
            except PermissionError:
                # Another concurrent codegen may have already installed the exact bytes we wanted.
                try:
                    if path.read_bytes() == encoded:
                        os.unlink(temporary)
                        return False
                except (FileNotFoundError, PermissionError):
                    pass
                if attempt == 9:
                    # Visual Studio/IntelliSense commonly keeps generated headers open with
                    # read/write sharing but without delete sharing, which blocks os.replace()
                    # indefinitely. The owning build cannot compile this header until this custom
                    # command completes, so use an in-place rewrite only as the final Windows
                    # sharing-mode fallback after a short atomic-replace grace period.
                    with path.open("r+b", buffering=0) as output:
                        view = memoryview(encoded)
                        offset = 0
                        while offset < len(view):
                            written = output.write(view[offset:])
                            if not written:
                                raise OSError("short write while updating generated SlugUI header")
                            offset += written
                        output.truncate(len(encoded))
                        os.fsync(output.fileno())
                    os.unlink(temporary)
                    return True
                time.sleep(0.05)
        return True
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compile a .slugui component into a typed C++ header."
    )
    parser.add_argument("input", type=Path, help="input .slugui file")
    parser.add_argument("-o", "--output", type=Path, help="generated C++ header")
    parser.add_argument("--namespace", default="slugia::generated", help="generated C++ namespace")
    parser.add_argument("--class-name", help="generated C++ class name override")
    parser.add_argument(
        "--strip-source-metadata", action="store_true",
        help="omit source/provider metadata from generated runtime C++ while preserving stable IDs")
    parser.add_argument("--check", action="store_true", help="parse and type-check without writing")
    parser.add_argument("--stdout", action="store_true", help="write generated C++ to stdout")
    arguments = parser.parse_args(argv)
    if not arguments.check and not arguments.stdout and arguments.output is None:
        parser.error("-o/--output is required unless --check or --stdout is used")
    try:
        source = arguments.input.read_text(encoding="utf-8")
        generated = compile_text(
            source, arguments.input.name, arguments.namespace, arguments.class_name,
            arguments.strip_source_metadata)
        if arguments.stdout:
            sys.stdout.write(generated)
        elif not arguments.check:
            write_atomic(arguments.output, generated)
        return 0
    except OSError as error:
        print(f"{arguments.input}: error: {error}", file=sys.stderr)
        return 2
    except CompileError as error:
        if error.token is None:
            location = str(arguments.input)
        else:
            location = f"{arguments.input}:{error.token.line}:{error.token.column}"
        print(f"{location}: error: {error.message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
