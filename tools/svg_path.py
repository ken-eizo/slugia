#!/usr/bin/env python3

"""Strict SVG path-data parser used only by the SlugUI AOT toolchain."""

from __future__ import annotations

from dataclasses import dataclass
import math
import re
from typing import Iterable


class SvgPathError(ValueError):
    pass


@dataclass(frozen=True)
class Operation:
    name: str
    values: tuple[float, ...] = ()


@dataclass(frozen=True)
class Bounds:
    minimum_x: float
    minimum_y: float
    maximum_x: float
    maximum_y: float

    @property
    def width(self) -> float:
        return self.maximum_x - self.minimum_x

    @property
    def height(self) -> float:
        return self.maximum_y - self.minimum_y


_COMMANDS = set("AaCcHhLlMmQqSsTtVvZz")
_NUMBER = r"[-+]?(?:(?:\d+\.?(?:\d*)?)|(?:\.\d+))(?:[eE][-+]?\d+)?"
_TOKEN = re.compile(rf"[AaCcHhLlMmQqSsTtVvZz]|{_NUMBER}")
_PARAMETERS = {
    "A": 7, "C": 6, "H": 1, "L": 2, "M": 2, "Q": 4,
    "S": 4, "T": 2, "V": 1,
}


def _tokens(data: str) -> list[str]:
    result: list[str] = []
    position = 0
    for match in _TOKEN.finditer(data):
        gap = data[position:match.start()]
        if gap.strip(" \t\r\n,"):
            raise SvgPathError(f"unexpected SVG path text at byte {position}")
        result.append(match.group(0))
        position = match.end()
    if data[position:].strip(" \t\r\n,"):
        raise SvgPathError(f"unexpected SVG path text at byte {position}")
    return result


def _angle(u: tuple[float, float], v: tuple[float, float]) -> float:
    dot = u[0] * v[0] + u[1] * v[1]
    cross = u[0] * v[1] - u[1] * v[0]
    return math.atan2(cross, dot)


def _arc_cubics(
    start: tuple[float, float],
    rx: float,
    ry: float,
    rotation_degrees: float,
    large_arc: float,
    sweep: float,
    end: tuple[float, float],
) -> list[Operation]:
    if large_arc not in (0.0, 1.0) or sweep not in (0.0, 1.0):
        raise SvgPathError("arc flags must be 0 or 1")
    rx, ry = abs(rx), abs(ry)
    if rx <= 1.0e-12 or ry <= 1.0e-12:
        return [Operation("lineTo", end)]
    if abs(start[0] - end[0]) <= 1.0e-12 and abs(start[1] - end[1]) <= 1.0e-12:
        return []

    phi = math.radians(rotation_degrees % 360.0)
    cosine, sine = math.cos(phi), math.sin(phi)
    half_x = (start[0] - end[0]) * 0.5
    half_y = (start[1] - end[1]) * 0.5
    x_prime = cosine * half_x + sine * half_y
    y_prime = -sine * half_x + cosine * half_y

    scale = x_prime * x_prime / (rx * rx) + y_prime * y_prime / (ry * ry)
    if scale > 1.0:
        factor = math.sqrt(scale)
        rx *= factor
        ry *= factor

    numerator = max(
        0.0,
        rx * rx * ry * ry - rx * rx * y_prime * y_prime -
        ry * ry * x_prime * x_prime,
    )
    denominator = max(
        rx * rx * y_prime * y_prime + ry * ry * x_prime * x_prime,
        1.0e-30,
    )
    coefficient = math.sqrt(numerator / denominator)
    if bool(large_arc) == bool(sweep):
        coefficient = -coefficient
    cx_prime = coefficient * (rx * y_prime / ry)
    cy_prime = coefficient * (-ry * x_prime / rx)
    center = (
        cosine * cx_prime - sine * cy_prime + (start[0] + end[0]) * 0.5,
        sine * cx_prime + cosine * cy_prime + (start[1] + end[1]) * 0.5,
    )

    unit_start = ((x_prime - cx_prime) / rx, (y_prime - cy_prime) / ry)
    unit_end = ((-x_prime - cx_prime) / rx, (-y_prime - cy_prime) / ry)
    start_angle = _angle((1.0, 0.0), unit_start)
    sweep_angle = _angle(unit_start, unit_end)
    if not sweep and sweep_angle > 0.0:
        sweep_angle -= math.tau
    elif sweep and sweep_angle < 0.0:
        sweep_angle += math.tau
    segments = max(1, int(math.ceil(abs(sweep_angle) / (math.pi * 0.5))))
    delta = sweep_angle / segments

    def transform(x: float, y: float) -> tuple[float, float]:
        return (
            center[0] + cosine * rx * x - sine * ry * y,
            center[1] + sine * rx * x + cosine * ry * y,
        )

    result: list[Operation] = []
    angle = start_angle
    for _ in range(segments):
        next_angle = angle + delta
        alpha = 4.0 / 3.0 * math.tan(delta * 0.25)
        p0 = (math.cos(angle), math.sin(angle))
        p1 = (p0[0] - alpha * p0[1], p0[1] + alpha * p0[0])
        p3 = (math.cos(next_angle), math.sin(next_angle))
        p2 = (p3[0] + alpha * p3[1], p3[1] - alpha * p3[0])
        c1, c2, target = transform(*p1), transform(*p2), transform(*p3)
        result.append(Operation("cubicTo", (*c1, *c2, *target)))
        angle = next_angle
    return result


def operation_bounds(operations: Iterable[Operation]) -> Bounds | None:
    """Return the exact drawable bounds of normalized path operations."""
    minimum_x = minimum_y = math.inf
    maximum_x = maximum_y = -math.inf
    current = (0.0, 0.0)
    subpath = (0.0, 0.0)
    has_drawable_segment = False

    def include(point: tuple[float, float]) -> None:
        nonlocal minimum_x, minimum_y, maximum_x, maximum_y
        minimum_x = min(minimum_x, point[0])
        minimum_y = min(minimum_y, point[1])
        maximum_x = max(maximum_x, point[0])
        maximum_y = max(maximum_y, point[1])

    def quadratic(point0: float, point1: float, point2: float, t: float) -> float:
        inverse = 1.0 - t
        return inverse * inverse * point0 + 2.0 * inverse * t * point1 + t * t * point2

    def cubic(point0: float, point1: float, point2: float, point3: float,
              t: float) -> float:
        inverse = 1.0 - t
        return (
            inverse * inverse * inverse * point0 +
            3.0 * inverse * inverse * t * point1 +
            3.0 * inverse * t * t * point2 +
            t * t * t * point3
        )

    def cubic_extrema(point0: float, point1: float, point2: float,
                      point3: float) -> list[float]:
        a = -point0 + 3.0 * point1 - 3.0 * point2 + point3
        b = 2.0 * (point0 - 2.0 * point1 + point2)
        c = point1 - point0
        if abs(a) <= 1.0e-12:
            if abs(b) <= 1.0e-12:
                return []
            root = -c / b
            return [root] if 0.0 < root < 1.0 else []
        discriminant = b * b - 4.0 * a * c
        if discriminant < 0.0:
            return []
        root = math.sqrt(max(0.0, discriminant))
        values = [(-b - root) / (2.0 * a), (-b + root) / (2.0 * a)]
        return [value for value in values if 0.0 < value < 1.0]

    for operation in operations:
        if operation.name == "moveTo":
            current = (operation.values[0], operation.values[1])
            subpath = current
            continue

        start = current
        if operation.name == "lineTo":
            current = (operation.values[0], operation.values[1])
            include(start)
            include(current)
        elif operation.name == "quadraticTo":
            control = (operation.values[0], operation.values[1])
            current = (operation.values[2], operation.values[3])
            include(start)
            include(current)
            for axis in (0, 1):
                denominator = start[axis] - 2.0 * control[axis] + current[axis]
                if abs(denominator) <= 1.0e-12:
                    continue
                t = (start[axis] - control[axis]) / denominator
                if 0.0 < t < 1.0:
                    point = [0.0, 0.0]
                    point[axis] = quadratic(start[axis], control[axis], current[axis], t)
                    other = 1 - axis
                    point[other] = quadratic(
                        start[other], control[other], current[other], t,
                    )
                    include((point[0], point[1]))
        elif operation.name == "cubicTo":
            control1 = (operation.values[0], operation.values[1])
            control2 = (operation.values[2], operation.values[3])
            current = (operation.values[4], operation.values[5])
            include(start)
            include(current)
            candidates = set(cubic_extrema(
                start[0], control1[0], control2[0], current[0],
            ))
            candidates.update(cubic_extrema(
                start[1], control1[1], control2[1], current[1],
            ))
            for t in candidates:
                include((
                    cubic(start[0], control1[0], control2[0], current[0], t),
                    cubic(start[1], control1[1], control2[1], current[1], t),
                ))
        elif operation.name == "close":
            current = subpath
            include(start)
            include(current)
        else:
            raise SvgPathError(f"unknown normalized SVG operation {operation.name}")
        has_drawable_segment = True

    if not has_drawable_segment:
        return None
    return Bounds(minimum_x, minimum_y, maximum_x, maximum_y)


def parse_svg_path(data: str) -> list[Operation]:
    tokens = _tokens(data)
    if not tokens:
        raise SvgPathError("SVG path is empty")
    operations: list[Operation] = []
    index = 0
    command: str | None = None
    current = (0.0, 0.0)
    subpath = (0.0, 0.0)
    cubic_control: tuple[float, float] | None = None
    quadratic_control: tuple[float, float] | None = None

    while index < len(tokens):
        if tokens[index] in _COMMANDS:
            command = tokens[index]
            index += 1
            if command in "Zz":
                operations.append(Operation("close"))
                current = subpath
                cubic_control = None
                quadratic_control = None
                command = None
                continue
        if command is None:
            raise SvgPathError("SVG path data must begin with a command")

        upper = command.upper()
        count = _PARAMETERS[upper]
        if index + count > len(tokens) or any(
            token in _COMMANDS for token in tokens[index:index + count]
        ):
            raise SvgPathError(f"command {command} has too few parameters")
        try:
            values = [float(token) for token in tokens[index:index + count]]
        except ValueError as error:
            raise SvgPathError(f"command {command} contains an invalid number") from error
        if not all(math.isfinite(value) for value in values):
            raise SvgPathError("SVG path numbers must be finite")
        index += count
        relative = command.islower()

        def point(x: float, y: float) -> tuple[float, float]:
            return (x + current[0], y + current[1]) if relative else (x, y)

        if upper == "M":
            current = point(values[0], values[1])
            subpath = current
            operations.append(Operation("moveTo", current))
            cubic_control = None
            quadratic_control = None
            command = "l" if relative else "L"
        elif upper == "L":
            current = point(values[0], values[1])
            operations.append(Operation("lineTo", current))
            cubic_control = None
            quadratic_control = None
        elif upper == "H":
            current = (current[0] + values[0], current[1]) if relative else (values[0], current[1])
            operations.append(Operation("lineTo", current))
            cubic_control = None
            quadratic_control = None
        elif upper == "V":
            current = (current[0], current[1] + values[0]) if relative else (current[0], values[0])
            operations.append(Operation("lineTo", current))
            cubic_control = None
            quadratic_control = None
        elif upper == "C":
            c1 = point(values[0], values[1])
            c2 = point(values[2], values[3])
            current = point(values[4], values[5])
            operations.append(Operation("cubicTo", (*c1, *c2, *current)))
            cubic_control = c2
            quadratic_control = None
        elif upper == "S":
            c1 = (
                (2.0 * current[0] - cubic_control[0], 2.0 * current[1] - cubic_control[1])
                if cubic_control is not None else current
            )
            c2 = point(values[0], values[1])
            current = point(values[2], values[3])
            operations.append(Operation("cubicTo", (*c1, *c2, *current)))
            cubic_control = c2
            quadratic_control = None
        elif upper == "Q":
            control = point(values[0], values[1])
            current = point(values[2], values[3])
            operations.append(Operation("quadraticTo", (*control, *current)))
            quadratic_control = control
            cubic_control = None
        elif upper == "T":
            control = (
                (2.0 * current[0] - quadratic_control[0],
                 2.0 * current[1] - quadratic_control[1])
                if quadratic_control is not None else current
            )
            current = point(values[0], values[1])
            operations.append(Operation("quadraticTo", (*control, *current)))
            quadratic_control = control
            cubic_control = None
        elif upper == "A":
            target = point(values[5], values[6])
            operations.extend(_arc_cubics(
                current, values[0], values[1], values[2], values[3], values[4], target,
            ))
            current = target
            cubic_control = None
            quadratic_control = None
        else:
            raise SvgPathError(f"unsupported SVG command {command}")
    return operations
