#!/usr/bin/env python3

import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "tools"))

import slugui_compiler as compiler  # noqa: E402


class SlugUiCompilerTest(unittest.TestCase):
    def test_example_is_deterministic_and_typed(self):
        source_path = REPOSITORY / "examples" / "slugui_demo.slugui"
        source = source_path.read_text(encoding="utf-8")
        first = compiler.compile_text(source, source_path.name, "slugia::example")
        second = compiler.compile_text(source, source_path.name, "slugia::example")
        self.assertEqual(first, second)
        self.assertIn("struct SlugUiDemoGenerated", first)
        self.assertIn("Built result;", first)
        self.assertIn('hashId("SlugUiDemoGenerated/root")', first)
        self.assertNotIn("SlugUiDemoGenerated//root", first)
        self.assertIn("Property<std::int64_t> clicks", first)
        self.assertIn("ValueSource<slugia::Paint>", first)
        self.assertIn("struct SharedDefinition", first)
        self.assertIn("makeSharedDefinition", first)
        self.assertIn("ComponentInstance instantiate", first)
        self.assertIn("std::make_shared<const slugia::slugui::ComponentDefinition>", first)
        self.assertIn("callback_toggle_popup", first)
        self.assertIn("ImportFidelity::Native", first)

    def test_atomic_output_preserves_unchanged_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "generated.hpp"
            self.assertTrue(compiler.write_atomic(output, "first\n"))
            first_mtime = output.stat().st_mtime_ns
            self.assertFalse(compiler.write_atomic(output, "first\n"))
            self.assertEqual(output.stat().st_mtime_ns, first_mtime)
            self.assertTrue(compiler.write_atomic(output, "second\n"))
            self.assertEqual(output.read_text(encoding="utf-8"), "second\n")

    def test_reference_type_mismatch_is_rejected(self):
        source = """
component Invalid {
  property float amount = 1;
  Text root {
    text: $amount;
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "expected string"):
            compiler.compile_text(source, "invalid.slugui", "test")

    def test_unknown_attribute_is_rejected(self):
        source = """
component Invalid {
  Rectangle root {
    magic-shadow: true;
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "not valid"):
            compiler.compile_text(source, "invalid.slugui", "test")

    def test_comments_gradients_and_individual_corners(self):
        source = """
component PaintSample {
  // Values remain compile-time constants.
  Rectangle root {
    fill: radial(#ffffff, #102040, 0.5, 0.5, 1, 0.5, 0.8);
    radius: [4, 8, 12, 16];
    smoothing: [0%, 25%, 50%, 100%];
  }
}
"""
        generated = compiler.compile_text(source, "paint.slugui", "test")
        self.assertIn("GradientKind::Radial", generated)
        self.assertIn("CornerRadii{4.0f, 8.0f, 12.0f, 16.0f}", generated)
        self.assertIn("CornerSmoothing{0.0f, 25.0f, 50.0f, 100.0f}", generated)

    def test_unicode_is_emitted_as_utf8_bytes(self):
        source = """
component UnicodeSample {
  Text root {
    text: "日本語 😀";
  }
}
"""
        generated = compiler.compile_text(source, "unicode.slugui", "test")
        self.assertNotIn(r"\ud83d", generated.lower())
        self.assertIn(r"\360\237\230\200", generated)

    def test_transparent_eight_digit_color_emits_valid_cpp_float(self):
        source = """
component Transparent {
  Rectangle root {
    fill: #00000000;
  }
}
"""
        generated = compiler.compile_text(source, "transparent.slugui", "test")
        self.assertIn("Color::fromRgb8(0x000000, 0.0f)", generated)
        self.assertNotIn(", 0f)", generated)

    def test_rectangle_stroke_paint_width_and_alignment_are_typed(self):
        source = """
component BorderSample {
  Rectangle root {
    fill: #101820;
    stroke: linear(#ff0080, #00e5ff);
    stroke-width: 2.5;
    stroke-align: inside;
  }
}
"""
        generated = compiler.compile_text(source, "border.slugui", "test")
        self.assertIn(".stroke.paint.normal = slugia::Paint::gradient", generated)
        self.assertIn(".stroke.width = 2.5f", generated)
        self.assertIn(".stroke.align = slugia::StrokeAlign::Inside", generated)

    def test_rectangle_individual_stroke_widths_are_typed(self):
        source = """
component BorderSides {
  Rectangle root {
    width: 80px;
    height: 40px;
    stroke: #ffffff;
    stroke-width: [1, 2, 3, 4];
    stroke-align: outside;
  }
}
"""
        generated = compiler.compile_text(source, "border-sides.slugui", "test")
        self.assertIn(
            "BorderWidths{1.0f, 2.0f, 3.0f, 4.0f}", generated)
        self.assertIn(".stroke.individualWidths", generated)
        self.assertIn("StrokeAlign::Outside", generated)

    def test_rectangle_individual_stroke_widths_can_bind(self):
        source = """
component BoundBorderSides {
  property border-widths sides = [1, 2, 3, 4];
  Rectangle root {
    stroke: #ffffff;
    stroke-width: $sides;
  }
}
"""
        generated = compiler.compile_text(source, "bound-border-sides.slugui", "test")
        self.assertIn("Property<slugia::BorderWidths> sides", generated)
        self.assertIn("ValueSource<slugia::BorderWidths>{result.sides}", generated)

    def test_svg_path_data_generates_prebuild_atlas_assets(self):
        source = """
component VectorAsset {
  Shape icon {
    width: 32px;
    height: 32px;
    path-data: ["M1 1 h20 v20 h-20 z", "M4 16 Q10 4 16 16 T28 16"];
    fill: #80d8ff;
    stroke-path-data: "M2 2 C8 0 20 0 26 2 S30 20 26 26 A4 4 0 0 1 22 30";
    stroke: #ff4080;
    stroke-width: 2;
    stroke-align: center;
  }
}
"""
        generated = compiler.compile_text(source, "vector.slugui", "test")
        self.assertIn("explicit VectorAsset(slugia::VectorAtlas& atlas)", generated)
        self.assertGreaterEqual(generated.count("atlas->addPath"), 2)
        self.assertGreaterEqual(generated.count(".svgPathYDown("), 3)
        self.assertIn("M1 1 h20 v20 h-20 z", generated)
        self.assertIn("M4 16 Q10 4 16 16 T28 16", generated)
        self.assertIn("M2 2 C8 0 20 0 26 2 S30 20 26 26 A4 4 0 0 1 22 30", generated)
        self.assertNotIn(".lineTo(", generated)
        self.assertNotIn(".quadraticTo(", generated)
        self.assertNotIn(".cubicTo(", generated)
        self.assertIn(".strokeShape = asset_", generated)
        self.assertIn(".fillPlacement = slugia::Rect{", generated)
        self.assertIn(".strokePlacement = slugia::Rect{", generated)
        self.assertNotIn("parse_svg", generated)

    def test_evenodd_winding_is_normalized_before_atlas_build(self):
        source = """
component EvenOddAsset {
  Shape icon {
    width: 20px;
    height: 20px;
    path-data: "M0 0 L20 0 L20 20 L0 20 Z M5 5 L15 5 L15 15 L5 15 Z";
    path-winding-rules: evenodd;
    fill: #ffffff;
  }
}
"""
        generated = compiler.compile_text(source, "evenodd.slugui", "test")
        self.assertIn(".svgPathYDown(", generated)
        self.assertNotIn(".normalizeEvenOdd();", generated)
        self.assertIn("slugia::FillRule::EvenOdd", generated)
        self.assertIn("atlas->addPath", generated)

    def test_mixed_winding_rules_fail_instead_of_approximating(self):
        source = """
component MixedWinding {
  Shape icon {
    width: 20px;
    height: 20px;
    path-data: ["M0 0 L20 0 L20 20 L0 20 Z", "M5 5 L15 5 L15 15 L5 15 Z"];
    path-winding-rules: [nonzero, evenodd];
    fill: #ffffff;
  }
}
"""
        with self.assertRaises(compiler.CompileError) as caught:
            compiler.compile_text(source, "mixed-winding.slugui", "test")
        self.assertIn("mixed nonzero/evenodd", caught.exception.message)

    def test_identical_vector_geometry_is_interned_once(self):
        source = """
component RepeatedIcons {
  Absolute root {
    Shape first {
      width: 16px;
      height: 16px;
      path-data: "M0 0 L16 0 L16 16 L0 16 Z";
      fill: #ffffff;
    }
    Shape second {
      x: 20px;
      width: 16px;
      height: 16px;
      path-data: "M0 0 L16 0 L16 16 L0 16 Z";
      fill: #ff0000;
    }
  }
}
"""
        generated = compiler.compile_text(source, "repeated-icons.slugui", "test")
        self.assertEqual(generated.count("atlas->addPath"), 1)
        self.assertGreaterEqual(generated.count(", asset_0,"), 2)

    def test_explicit_vector_placements_override_path_bounds(self):
        source = """
component PlacedVector {
  Shape icon {
    width: 20px;
    height: 10px;
    path-data: "M0 0 L4 0 L4 4 Z";
    path-placement: [2px, 1px, 8px, 6px];
    fill: #ffffff;
  }
}
"""
        generated = compiler.compile_text(source, "placed-vector.slugui", "test")
        self.assertIn("fillPlacement = slugia::Rect{0.1f, 0.1f, 0.4f, 0.6f}", generated)

    def test_invalid_svg_path_is_rejected_at_aot_time(self):
        source = """
component InvalidVector {
  Shape icon {
    path-data: "M 0";
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "too few parameters"):
            compiler.compile_text(source, "invalid-vector.slugui", "test")

    def test_checked_in_figma_fixture_compiles(self):
        source_path = REPOSITORY / "examples" / "figma_sample.slugui"
        generated = compiler.compile_text(
            source_path.read_text(encoding="utf-8"), source_path.name,
            "slugia::example", "FigmaSampleGenerated")
        self.assertIn("struct FigmaSampleGenerated", generated)
        self.assertIn("slugia::hashId(", generated)

    def test_generated_class_override_keeps_component_stable_ids(self):
        source = """
component FigmaSelectionGenerated {
  Absolute selection_root {
    Text label {
      text: "Weighted";
      font: "Geist";
      font-style: "SemiBold";
      font-weight: 650;
    }
  }
}
"""
        generated = compiler.compile_text(
            source, "selection.slugui", "slugia::example", "FigmaSampleGenerated")
        self.assertIn("struct FigmaSampleGenerated", generated)
        self.assertIn('hashId("FigmaSelectionGenerated/selection_root/label")', generated)
        self.assertIn(".style.weight = 650;", generated)
        self.assertIn('.style.fontStyle = "SemiBold";', generated)
        self.assertIn('atlas->loadSystemFont("Geist", 650, false', generated)
        self.assertIn('"SemiBold");', generated)
        self.assertIn("87u", generated)  # 'W' from the actually used glyph set
        self.assertIn('atlas->prepareText(', generated)
        self.assertIn('"Geist", 650, false, "SemiBold"', generated)

    def test_figma_node_id_is_stable_across_rename_and_regroup(self):
        first = """
component FirstImport {
  Absolute old_group {
    Text old_name {
      text: "A";
      source-provider: "figma";
      source-document: "fixture-file";
      source-node: "42:7";
    }
  }
}
"""
        second = """
component RenamedImport {
  Absolute new_group {
    Absolute extra_group {
      Text renamed_node {
        text: "A";
        source-provider: "figma";
        source-document: "fixture-file";
        source-node: "42:7";
      }
    }
  }
}
"""
        first_cpp = compiler.compile_text(first, "first.slugui", "test")
        second_cpp = compiler.compile_text(second, "second.slugui", "test")
        stable = 'hashId("figma/fixture-file/42:7")'
        self.assertIn(stable, first_cpp)
        self.assertIn(stable, second_cpp)
        self.assertNotIn('hashId("FirstImport/old_group/old_name")', first_cpp)

    def test_figma_provider_data_is_preserved(self):
        source = r"""
component ProviderData {
  Absolute instance {
    source-provider: "figma";
    source-document: "fixture-file";
    source-node: "90:3";
    source-provider-data: "{\"componentProperties\":{\"State\":{\"type\":\"VARIANT\",\"value\":\"Focus\"}},\"componentSetId\":\"90:1\",\"constraints\":{\"horizontal\":\"MAX\",\"vertical\":\"CENTER\"},\"mainComponentId\":\"90:2\",\"nodeType\":\"INSTANCE\",\"variantSelection\":{\"State\":\"Focus\"}}";
  }
}
"""
        generated = compiler.compile_text(source, "provider-data.slugui", "test")
        self.assertIn(".source.providerData =", generated)
        self.assertIn(r"componentSetId", generated)
        self.assertIn(r"mainComponentId", generated)
        self.assertIn('.source.componentSetId = std::string{"90:1"};', generated)
        self.assertIn('.source.mainComponentId = std::string{"90:2"};', generated)
        self.assertIn('.source.nodeType = std::string{"INSTANCE"};', generated)
        self.assertIn(
            ".source.figmaKind = slugia::slugui::FigmaSemanticKind::Instance;", generated)
        self.assertIn('.source.variantSelectionJson = std::string{', generated)
        self.assertIn(r'\"State\":\"Focus\"', generated)
        self.assertIn(".layout.horizontalConstraint = slugia::slugui::Constraint::Max;", generated)
        self.assertIn(".layout.verticalConstraint = slugia::slugui::Constraint::Center;", generated)

    def test_preview_can_strip_figma_source_metadata_without_losing_layout_or_stable_id(self):
        source = r"""
component ProviderData {
  Absolute instance {
    source-provider: "figma";
    source-document: "fixture-file";
    source-node: "90:3";
    source-provider-data: "{\"constraints\":{\"horizontal\":\"MAX\",\"vertical\":\"CENTER\"},\"nodeType\":\"INSTANCE\",\"mainComponentId\":\"90:2\"}";
  }
}
"""
        generated = compiler.compile_text(
            source, "provider-data.slugui", "test", strip_source_metadata=True)
        self.assertIn('hashId("figma/fixture-file/90:3")', generated)
        self.assertIn(".layout.horizontalConstraint = slugia::slugui::Constraint::Max;", generated)
        self.assertIn(".layout.verticalConstraint = slugia::slugui::Constraint::Center;", generated)
        self.assertNotIn(".source.providerData", generated)
        self.assertNotIn(".source.mainComponentId", generated)
        self.assertNotIn(".source.nodeType", generated)

    def test_large_provider_data_is_chunked_for_msvc(self):
        payload = "x" * 20000
        source = f"""
component LargeMetadata {{
  Absolute instance {{
    source-provider: "figma";
    source-provider-data: "{payload}";
  }}
}}
"""
        generated = compiler.compile_text(source, "large-provider-data.slugui", "test")
        self.assertIn(".source.providerData = std::string{", generated)
        self.assertIn(".source.providerData.append(", generated)
        self.assertNotIn('"' + payload + '"', generated)

    def test_legacy_figma_instance_swap_catalog_is_compacted(self):
        payload = (
            '{"componentDefinitions":{"Icon":{"type":"INSTANCE_SWAP","defaultValue":"90:icon",'
            '"preferredValues":[{"type":"COMPONENT","key":"a"},{"type":"COMPONENT","key":"b"}]}},'
            '"componentProperties":{"Icon":{"type":"INSTANCE_SWAP","value":"90:selected"},'
            '"State":{"type":"VARIANT","value":"Focus"}},'
            '"mainComponentId":"90:2","nodeType":"INSTANCE"}'
        )
        compacted = compiler.compact_figma_provider_data(payload)
        self.assertNotIn("preferredValues", compacted)
        self.assertIn('"preferredValueCount":2', compacted)
        self.assertIn('"value":"90:selected"', compacted)
        self.assertIn('"variantSelection":{"State":"Focus"}', compacted)
        self.assertIn('"mainComponentId":"90:2"', compacted)

    def test_figma_image_placeholder_attributes_are_typed(self):
        source = """
component ImagePlaceholder {
  Rectangle image {
    width: 320px;
    height: 240px;
    image-source-width: 1920;
    image-source-height: 1080;
    image-scale-mode: fit;
    fill: #405060;
  }
}
"""
        generated = compiler.compile_text(source, "image-placeholder.slugui", "test")
        self.assertIn(".image.enabled = true;", generated)
        self.assertIn(".image.sourceWidth = 1920.0f;", generated)
        self.assertIn(".image.sourceHeight = 1080.0f;", generated)
        self.assertIn(".image.scaleMode = slugia::slugui::ImageScaleMode::Fit;", generated)

    def test_figma_stretch_shape_can_use_preferred_extent_for_path_placement(self):
        source = """
component StretchVector {
  Row root {
    width: 100px;
    height: 40px;
    Shape divider {
      width: 1px;
      preferred-height: 32px;
      align-self: stretch;
      path-data: "M1 0 L0 0 L0 32 L1 32 L1 0 Z";
      path-placement: [0px, 0px, 1px, 32px];
      source-provider: "figma";
    }
  }
}
"""
        generated = compiler.compile_text(source, "stretch-vector.slugui", "test")
        self.assertIn("fillPlacement = slugia::Rect{0.0f, 0.0f, 1.0f, 1.0f}", generated)

    def test_wrap_layout_attributes_are_typed(self):
        source = """
component WrapLayout {
  Row root {
    wrap: true;
    spacing: 10px;
    counter-spacing: 20px;
    counter-justify: space-between;
  }
}
"""
        generated = compiler.compile_text(source, "wrap.slugui", "test")
        self.assertIn(".layout.wrap = true;", generated)
        self.assertIn(".layout.counterSpacing = slugia::slugui::Length::logical(20.0f)", generated)
        self.assertIn(".layout.counterAlignment = slugia::slugui::Justify::SpaceBetween;", generated)

    def test_figma_constraints_are_typed(self):
        source = """
component Constraints {
  Absolute root {
    Rectangle child {
      x: -4px;
      y: 5px;
      width: 22px;
      height: 22px;
      constraint-horizontal: stretch;
      constraint-vertical: center;
    }
  }
}
"""
        generated = compiler.compile_text(source, "constraints.slugui", "test")
        self.assertIn(".layout.horizontalConstraint = slugia::slugui::Constraint::Stretch;", generated)
        self.assertIn(".layout.verticalConstraint = slugia::slugui::Constraint::Center;", generated)

    def test_absolute_position_attribute_is_typed(self):
        source = """
component AbsoluteChild {
  Row root {
    width: 200px;
    height: 80px;
    Rectangle floating {
      position: absolute;
      x: 120px;
      y: 10px;
      width: 30px;
      height: 20px;
    }
  }
}
"""
        generated = compiler.compile_text(source, "absolute-child.slugui", "test")
        self.assertIn(".layout.absolutePositioned = true;", generated)
        self.assertIn(".layout.x = slugia::slugui::Length::logical(120.0f)", generated)
        self.assertIn(".layout.y = slugia::slugui::Length::logical(10.0f)", generated)

    def test_reverse_paint_order_attribute_is_typed(self):
        source = """
component ReversePaint {
  Row root {
    reverse-paint-order: true;
    Rectangle first { width: 20px; height: 20px; }
    Rectangle second { width: 20px; height: 20px; }
  }
}
"""
        generated = compiler.compile_text(source, "reverse-paint.slugui", "test")
        self.assertIn(".layout.reverseChildPaintOrder = true;", generated)

    def test_generated_child_subtrees_have_bounded_cpp_lifetimes(self):
        source = """
component ScopeFixture {
  Column root {
    Rectangle first { width: 10px; }
    Rectangle second { width: 20px; }
  }
}
"""
        generated = compiler.compile_text(source, "scope-fixture.slugui", "test")
        self.assertIn(
            "auto built_node_1 = [&]() -> slugia::slugui::Element {\n"
            "      auto node_1 =",
            generated,
        )
        self.assertIn("return node_1;\n    }();\n    node_0.add(std::move(built_node_1));", generated)
        self.assertIn(
            "auto built_node_2 = [&]() -> slugia::slugui::Element {\n"
            "      auto node_2 =",
            generated,
        )
        self.assertIn("return node_2;\n    }();\n    node_0.add(std::move(built_node_2));", generated)

    def test_font_weight_range_is_checked(self):
        source = """
component InvalidWeight {
  Text label {
    text: "x";
    font-weight: 1200;
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "between 1 and 1000"):
            compiler.compile_text(source, "invalid-weight.slugui", "test")

    def test_mixed_text_runs_generate_typed_styles(self):
        source = """
component RichText {
  Text label {
    width: 200px;
    height: 40px;
    text: "Red blue";
    text-align: center;
    text-align-vertical: bottom;
    text-runs: [
      text-run("Red", "Geist", 16, 700, false, true, false, 0, 1.25, #ff0000),
      text-run(" blue", "Geist", 16, 400, true, false, false, 0.5, 1.25, #0000ff)
    ];
  }
}
"""
        generated = compiler.compile_text(source, "rich.slugui", "test")
        self.assertEqual(generated.count(".runs.push_back"), 2)
        self.assertIn('.fontName = "Geist"', generated)
        self.assertIn(".weight = 700", generated)
        self.assertIn(".weight = 400", generated)
        self.assertIn("VerticalAlign::Bottom", generated)
        self.assertIn("Color::fromRgb8(0xff0000)", generated)
        self.assertIn("Color::fromRgb8(0x0000ff)", generated)

    def test_mixed_text_runs_must_match_text(self):
        source = """
component InvalidRuns {
  Text label {
    text: "abc";
    text-runs: [text-run("ab", "Geist", 14, 400, false, false, false, 0, 1.25, #ffffff)];
  }
}
"""
        with self.assertRaisesRegex(compiler.CompileError, "exactly match"):
            compiler.compile_text(source, "invalid-runs.slugui", "test")

    def test_v1_figma_absolute_vector_placement_is_recentered(self):
        source = """
// SlugUI exchange format 1, generated by the Figma selection exporter.
component Legacy {
  Shape icon {
    width: 12px;
    height: 8px;
    stroke-path-data: "M0 0 L13.25 0 L13.25 9.25 Z";
    stroke-placement: [-284.625px, 227.375px, 13.25px, 9.25px];
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "legacy.slugui", "test")
        self.assertIn(
            "strokePlacement = slugia::Rect{-0.0520833333f, -0.078125f, "
            "1.10416667f, 1.15625f}", generated)

    def test_v2_disjoint_figma_vector_placement_is_recovered(self):
        source = """
// SlugUI exchange format 2, generated by the Figma selection exporter.
component BrokenV2 {
  Shape icon {
    width: 12px;
    height: 8px;
    stroke-path-data: "M0 0 L13.25 0 L13.25 9.25 Z";
    stroke-placement: [-284.625px, 227.375px, 13.25px, 9.25px];
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "broken-v2.slugui", "test")
        self.assertIn(
            "strokePlacement = slugia::Rect{-0.0520833333f, -0.078125f, "
            "1.10416667f, 1.15625f}", generated)

    def test_v2_local_figma_vector_placement_is_preserved(self):
        source = """
// SlugUI exchange format 2, generated by the Figma selection exporter.
component ValidV2 {
  Shape icon {
    width: 12px;
    height: 8px;
    stroke-path-data: "M0 0 L13.25 0 L13.25 9.25 Z";
    stroke-placement: [-0.625px, -0.625px, 13.25px, 9.25px];
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "valid-v2.slugui", "test")
        self.assertIn(
            "strokePlacement = slugia::Rect{-0.0520833333f, -0.078125f, "
            "1.10416667f, 1.15625f}", generated)

    def test_v2_zero_height_figma_line_recovers_render_bounds(self):
        source = """
// SlugUI exchange format 2, generated by the Figma selection exporter.
component FigmaLine {
  Shape divider {
    x: 10px;
    y: 20px;
    width: 23px;
    height: 0px;
    stroke-path-data: "M23 0 L23 6 L0 6 L0 0 L23 0 Z";
    stroke-placement: [0px, -3px, 23px, 6px];
    stroke-width: 6;
    stroke-align: outside;
    source-provider: "figma";
  }
}
"""
        generated = compiler.compile_text(source, "figma-line.slugui", "test")
        self.assertIn("strokePlacement = slugia::Rect{0.0f, 0.0f, 1.0f, 1.0f}", generated)
        self.assertIn("layout.x = slugia::slugui::Length::logical(10.0f)", generated)
        self.assertIn("layout.y = slugia::slugui::Length::logical(17.0f)", generated)
        self.assertIn("layout.width = slugia::slugui::Length::logical(23.0f)", generated)
        self.assertIn("layout.height = slugia::slugui::Length::logical(6.0f)", generated)
        self.assertNotIn("sourceStroke", generated)


if __name__ == "__main__":
    unittest.main()
