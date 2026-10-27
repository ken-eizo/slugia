#include "slugia/slugui.hpp"

#include "slugia/vector_atlas.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace slugia::slugui {
namespace {

bool same(Color a, Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool same(Paint a, Paint b) {
  return a.kind == b.kind && same(a.start, b.start) && same(a.end, b.end) &&
         a.origin.x == b.origin.x && a.origin.y == b.origin.y &&
         a.target.x == b.target.x && a.target.y == b.target.y &&
         a.opacity == b.opacity && a.shaderParameter == b.shaderParameter;
}

bool same(Length a, Length b) { return a.value == b.value && a.unit == b.unit; }

bool same(CornerRadii a, CornerRadii b) {
  return a.topLeft == b.topLeft && a.topRight == b.topRight &&
         a.bottomRight == b.bottomRight && a.bottomLeft == b.bottomLeft;
}

bool same(CornerSmoothing a, CornerSmoothing b) {
  return a.topLeftPercent == b.topLeftPercent && a.topRightPercent == b.topRightPercent &&
         a.bottomRightPercent == b.bottomRightPercent &&
         a.bottomLeftPercent == b.bottomLeftPercent;
}

bool same(BorderWidths a, BorderWidths b) {
  return a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom && a.left == b.left;
}

bool same(const PropertyValue& a, const PropertyValue& b) {
  if (a.index() != b.index()) return false;
  return std::visit([](const auto& left, const auto& right) {
    using Left = std::decay_t<decltype(left)>;
    using Right = std::decay_t<decltype(right)>;
    if constexpr (!std::is_same_v<Left, Right>) {
      return false;
    } else {
      if constexpr (std::is_same_v<Left, Color> || std::is_same_v<Left, Paint> ||
                    std::is_same_v<Left, Length> || std::is_same_v<Left, CornerRadii> ||
                    std::is_same_v<Left, CornerSmoothing> ||
                    std::is_same_v<Left, BorderWidths>) {
        return same(left, right);
      } else {
        return left == right;
      }
    }
  }, a, b);
}

PropertyType propertyType(const PropertyValue& value) {
  return std::visit([](const auto& entry) -> PropertyType {
    using T = std::decay_t<decltype(entry)>;
    if constexpr (std::is_same_v<T, bool>) return PropertyType::Boolean;
    else if constexpr (std::is_same_v<T, std::int64_t>) return PropertyType::Integer;
    else if constexpr (std::is_same_v<T, float>) return PropertyType::Scalar;
    else if constexpr (std::is_same_v<T, std::string>) return PropertyType::String;
    else if constexpr (std::is_same_v<T, Color>) return PropertyType::Color;
    else if constexpr (std::is_same_v<T, Paint>) return PropertyType::Paint;
    else if constexpr (std::is_same_v<T, Length>) return PropertyType::Length;
    else if constexpr (std::is_same_v<T, CornerRadii>) return PropertyType::CornerRadii;
    else if constexpr (std::is_same_v<T, CornerSmoothing>) return PropertyType::CornerSmoothing;
    else if constexpr (std::is_same_v<T, BorderWidths>) return PropertyType::BorderWidths;
    else static_assert(!sizeof(T), "Unhandled SlugUI property type");
  }, value);
}

} // namespace

struct PropertyStore::Impl {
  struct Slot {
    PropertyValue value;
    std::uint64_t revision = 1;
  };
  struct Binding {
    PropertyId target = 0;
    std::vector<PropertyId> dependencies;
    std::function<PropertyValue(const PropertyStore&)> function;
  };
  struct Schema {
    std::vector<std::string> namesById;
    std::unordered_map<std::string, PropertyId> names;
    std::vector<Binding> bindings;
    std::vector<std::vector<std::size_t>> dependents;
  };

  std::shared_ptr<Schema> schema = std::make_shared<Schema>();
  std::vector<Slot> slots;
  std::vector<bool> bindingQueued;
  std::deque<std::size_t> dirtyBindings;
  std::uint64_t generation = 0;

  void ensureUniqueSchema() {
    if (schema.use_count() != 1) schema = std::make_shared<Schema>(*schema);
  }
  void queueBinding(std::size_t index) {
    if (bindingQueued[index]) return;
    bindingQueued[index] = true;
    dirtyBindings.push_back(index);
  }
  void queueDependents(PropertyId property) {
    if (property == 0 || property > schema->dependents.size()) return;
    for (const auto binding : schema->dependents[property - 1]) queueBinding(binding);
  }
};

PropertyStore::PropertyStore() : impl_(std::make_unique<Impl>()) {}
PropertyStore::~PropertyStore() = default;
PropertyStore::PropertyStore(PropertyStore&&) noexcept = default;
PropertyStore& PropertyStore::operator=(PropertyStore&&) noexcept = default;

PropertyId PropertyStore::addProperty(std::string name, PropertyValue value) {
  if (name.empty()) throw std::invalid_argument("SlugUI property names cannot be empty");
  if (impl_->schema->names.contains(name))
    throw std::invalid_argument("Duplicate SlugUI property: " + name);
  impl_->ensureUniqueSchema();
  const auto id = static_cast<PropertyId>(impl_->slots.size() + 1);
  impl_->slots.push_back({std::move(value), 1});
  impl_->schema->namesById.push_back(std::move(name));
  impl_->schema->dependents.emplace_back();
  impl_->schema->names.emplace(impl_->schema->namesById.back(), id);
  ++impl_->generation;
  return id;
}

const PropertyValue& PropertyStore::value(PropertyId property) const {
  if (property == 0 || property > impl_->slots.size()) {
    throw std::out_of_range("Invalid SlugUI property id");
  }
  return impl_->slots[property - 1].value;
}

bool PropertyStore::setValue(PropertyId property, PropertyValue value) {
  if (propertyType(this->value(property)) != propertyType(value)) {
    throw std::invalid_argument("SlugUI property type mismatch");
  }
  auto& slot = impl_->slots[property - 1];
  if (same(slot.value, value)) return false;
  slot.value = std::move(value);
  ++slot.revision;
  ++impl_->generation;
  impl_->queueDependents(property);
  return true;
}

bool PropertyStore::set(PropertyId property, PropertyValue value) {
  return setValue(property, std::move(value));
}

bool PropertyStore::copy(
    PropertyId target, const PropertyStore& source, PropertyId sourceProperty) {
  if (type(target) != source.type(sourceProperty)) {
    throw std::invalid_argument("SlugUI property link type mismatch");
  }
  return setValue(target, source.value(sourceProperty));
}

void PropertyStore::addBinding(PropertyId target, std::vector<PropertyId> dependencies,
                               BindingFunction function) {
  (void)value(target);
  for (const auto dependency : dependencies) (void)value(dependency);
  impl_->ensureUniqueSchema();
  const auto index = impl_->schema->bindings.size();
  impl_->schema->bindings.push_back({target, std::move(dependencies), std::move(function)});
  impl_->bindingQueued.push_back(false);
  for (const auto dependency : impl_->schema->bindings.back().dependencies)
    impl_->schema->dependents[dependency - 1].push_back(index);
  impl_->queueBinding(index);
}

std::optional<PropertyId> PropertyStore::find(std::string_view name) const {
  const auto it = impl_->schema->names.find(std::string(name));
  if (it == impl_->schema->names.end()) return std::nullopt;
  return it->second;
}

PropertyType PropertyStore::type(PropertyId property) const { return propertyType(value(property)); }
std::string_view PropertyStore::name(PropertyId property) const {
  (void)value(property);
  return impl_->schema->namesById[property - 1];
}
std::uint64_t PropertyStore::revision(PropertyId property) const {
  (void)value(property);
  return impl_->slots[property - 1].revision;
}
std::uint64_t PropertyStore::generation() const { return impl_->generation; }

PropertyStore PropertyStore::clone() const {
  PropertyStore result;
  result.impl_->schema = impl_->schema;
  result.impl_->slots = impl_->slots;
  result.impl_->bindingQueued.assign(result.impl_->schema->bindings.size(), false);
  result.impl_->generation = impl_->generation;
  for (std::size_t index = 0; index < result.impl_->schema->bindings.size(); ++index)
    result.impl_->queueBinding(index);
  return result;
}

bool PropertyStore::evaluateBindings() {
  bool changed = false;
  std::size_t evaluations = 0;
  const std::size_t maximumEvaluations = std::max<std::size_t>(
    32, impl_->schema->bindings.size() * std::max<std::size_t>(4, impl_->slots.size() + 1));
  while (!impl_->dirtyBindings.empty()) {
    const auto index = impl_->dirtyBindings.front();
    impl_->dirtyBindings.pop_front();
    impl_->bindingQueued[index] = false;
    if (++evaluations > maximumEvaluations)
      throw std::runtime_error("SlugUI property bindings did not converge");
    const auto& binding = impl_->schema->bindings[index];
    auto next = binding.function(*this);
    if (propertyType(next) != type(binding.target))
      throw std::invalid_argument("SlugUI binding returned the wrong property type");
    changed |= setValue(binding.target, std::move(next));
  }
  return changed;
}

namespace {

Element container(ElementId id, LayoutKind kind, std::string name) {
  Element result;
  result.id = id;
  result.name = std::move(name);
  result.layout.kind = kind;
  return result;
}

} // namespace

Element absolute(ElementId id, std::string name) {
  return container(id, LayoutKind::Absolute, std::move(name));
}
Element row(ElementId id, std::string name) {
  return container(id, LayoutKind::Row, std::move(name));
}
Element column(ElementId id, std::string name) {
  return container(id, LayoutKind::Column, std::move(name));
}
Element stack(ElementId id, std::string name) {
  return container(id, LayoutKind::Stack, std::move(name));
}

Element roundedRectangle(ElementId id, Paint paint, CornerRadii radii,
                         CornerSmoothing smoothing, std::string name) {
  auto result = stack(id, std::move(name));
  result.visual = RoundedRectangleVisual{StatefulPaint{paint}, radii, smoothing, {}};
  return result;
}

Element shape(ElementId id, ShapeId shapeId, Paint paint, std::string name) {
  auto result = stack(id, std::move(name));
  result.visual = ShapeVisual{shapeId, StatefulPaint{paint}};
  return result;
}

Element text(ElementId id, std::string value, TextStyle style, std::string name) {
  auto result = absolute(id, std::move(name));
  const auto paint = style.paint;
  result.visual = TextVisual{std::move(value), std::move(style), StatefulPaint{paint}};
  return result;
}

Element instance(ElementId id, InstanceId localId,
                 std::shared_ptr<const ComponentDefinition> definition,
                 std::string name) {
  if (localId == 0)
    throw std::invalid_argument("SlugUI nested instance local id must be non-zero");
  if (!definition)
    throw std::invalid_argument("SlugUI nested instance requires a definition");
  auto result = absolute(id, std::move(name));
  result.visual = InstanceVisual{localId, std::move(definition), {}, {}};
  return result;
}

struct Component::Impl {
  std::unordered_map<CallbackId, std::vector<Callback>> callbacks;
};

Component::Component(Element root)
    : root_(std::move(root)), properties_(), impl_(std::make_unique<Impl>()) {}
Component::Component(Element root, PropertyStore properties)
    : root_(std::move(root)), properties_(std::move(properties)),
      impl_(std::make_unique<Impl>()) {}
Component::~Component() = default;
Component::Component(Component&&) noexcept = default;
Component& Component::operator=(Component&&) noexcept = default;

void Component::on(CallbackId callback, Callback handler) {
  if (callback == 0 || !handler) return;
  impl_->callbacks[callback].push_back(std::move(handler));
}

void Component::dispatch(CallbackId callback, const UiEvent& event) {
  const auto it = impl_->callbacks.find(callback);
  if (it == impl_->callbacks.end()) return;
  for (const auto& handler : it->second) handler(event);
}

ComponentDefinition::ComponentDefinition(Element root)
    : root_(std::move(root)), defaults_() {}
ComponentDefinition::ComponentDefinition(Element root, PropertyStore defaults)
    : root_(std::move(root)), defaults_(std::move(defaults)) {}
ComponentDefinition::ComponentDefinition(Component&& component)
    : root_(std::move(component.root_)), defaults_(std::move(component.properties_)) {}
ComponentDefinition::~ComponentDefinition() = default;
ComponentDefinition::ComponentDefinition(ComponentDefinition&&) noexcept = default;
ComponentDefinition& ComponentDefinition::operator=(ComponentDefinition&&) noexcept = default;
const Element& ComponentDefinition::root() const noexcept { return root_; }
const PropertyStore& ComponentDefinition::defaults() const noexcept { return defaults_; }

struct ComponentInstance::Impl {
  std::unordered_map<CallbackId, std::vector<Callback>> callbacks;
};

ComponentInstance::ComponentInstance(std::shared_ptr<const ComponentDefinition> definition,
                                     InstanceId instanceId)
    : definition_(std::move(definition)),
      instanceId_(instanceId),
      properties_(definition_ ? definition_->defaults().clone() : PropertyStore{}),
      impl_(std::make_unique<Impl>()) {
  if (!definition_) throw std::invalid_argument("SlugUI component instance requires a definition");
}
ComponentInstance::~ComponentInstance() = default;
ComponentInstance::ComponentInstance(ComponentInstance&&) noexcept = default;
ComponentInstance& ComponentInstance::operator=(ComponentInstance&&) noexcept = default;

void ComponentInstance::on(CallbackId callback, Callback handler) {
  if (callback == 0 || !handler) return;
  impl_->callbacks[callback].push_back(std::move(handler));
}

void ComponentInstance::dispatch(CallbackId callback, const UiEvent& event) {
  const auto it = impl_->callbacks.find(callback);
  if (it == impl_->callbacks.end()) return;
  for (const auto& handler : it->second) handler(event);
}

FrameInput FrameInput::from(const InputState& input) {
  return {input.cursorPosition(), input.cursorDelta(), input.mouse(MouseButton::Left)};
}

InstanceId composeInstanceId(InstanceId parent, InstanceId local) noexcept {
  std::uint64_t value = parent ^ (local + 0x9e3779b97f4a7c15ULL +
                                  (parent << 6U) + (parent >> 2U));
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  value ^= value >> 31U;
  return value == 0 ? 1 : value;
}

namespace {

Rect intersect(Rect a, Rect b) {
  const float left = std::max(a.x, b.x);
  const float top = std::max(a.y, b.y);
  const float right = std::min(a.x + a.width, b.x + b.width);
  const float bottom = std::min(a.y + a.height, b.y + b.height);
  return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

struct Measured {
  float width = 0.0f;
  float height = 0.0f;
};

} // namespace

struct Runtime::Impl {
  struct Resolved {
    LayoutBox box;
    const Element* element = nullptr;
    bool enabled = true;
  };
  struct FlowChild {
    const Element* element = nullptr;
    Measured measured;
    float grow = 0.0f;
    Rect bounds = {};
  };
  struct MeasureKey {
    const Element* element = nullptr;
    std::uint32_t parentWidth = 0;
    std::uint32_t parentHeight = 0;
    bool operator==(const MeasureKey&) const = default;
  };
  struct MeasureKeyHash {
    std::size_t operator()(const MeasureKey& key) const noexcept {
      std::size_t hash = std::hash<const Element*>{}(key.element);
      hash ^= static_cast<std::size_t>(key.parentWidth) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
      hash ^= static_cast<std::size_t>(key.parentHeight) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
      return hash;
    }
  };
  struct RuntimeNodeIdHash {
    std::size_t operator()(RuntimeNodeId node) const noexcept {
      std::size_t hash = std::hash<InstanceId>{}(node.instance);
      hash ^= std::hash<ElementId>{}(node.element) + 0x9e3779b97f4a7c15ULL +
              (hash << 6U) + (hash >> 2U);
      return hash;
    }
  };

  std::vector<Resolved> resolved;
  std::vector<LayoutBox> publicBoxes;
  std::vector<std::vector<FlowChild>> flowScratch;
  std::unordered_map<MeasureKey, Measured, MeasureKeyHash> measureCache;
  std::unordered_map<RuntimeNodeId, std::size_t, RuntimeNodeIdHash> resolvedIndex;
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> hitCells;
  std::vector<std::size_t> hitGlobals;
  RuntimeNodeId hovered = {};
  RuntimeNodeId active = {};
  RuntimeNodeId focused = {};
  InstanceId currentInstanceId = 0;
  float scale = 1.0f;
  RuntimeStats stats = {};
  const void* layoutSubject = nullptr;
  std::uint64_t layoutStructureGeneration = 0;
  Rect layoutViewport = {};
  float layoutDeviceScale = 0.0f;
  std::uint64_t layoutPropertyGeneration = 0;
  std::vector<PropertyId> layoutDependencies;
  std::vector<std::uint64_t> layoutObserved;
  std::vector<std::size_t> changedLayoutDependencies;
  std::unordered_map<PropertyId, std::vector<const Element*>> layoutDependents;
  std::unordered_map<const Element*, const Element*> layoutParents;
  std::unordered_set<const Element*> dirtyMeasureElements;
  bool fullMeasureInvalidation = true;
  const void* treeDepthSubject = nullptr;
  std::uint64_t treeDepthStructureGeneration = 0;
  std::size_t cachedTreeDepth = 0;

  template <typename T>
  static void addLayoutDependency(
      std::vector<PropertyId>& result, const ValueSource<T>& source,
      const Element& element,
      std::unordered_map<PropertyId, std::vector<const Element*>>& dependents) {
    if (const auto property = source.propertyId()) {
      result.push_back(*property);
      dependents[*property].push_back(&element);
    }
  }

  static void collectLayoutDependencies(
      const Element& element, std::vector<PropertyId>& result,
      std::unordered_map<PropertyId, std::vector<const Element*>>& dependents,
      std::unordered_map<const Element*, const Element*>& parents,
      const Element* parent = nullptr) {
    parents[&element] = parent;
    addLayoutDependency(result, element.visible, element, dependents);
    addLayoutDependency(result, element.interaction.enabled, element, dependents);
    const auto& layout = element.layout;
    addLayoutDependency(result, layout.x, element, dependents);
    addLayoutDependency(result, layout.y, element, dependents);
    addLayoutDependency(result, layout.width, element, dependents);
    addLayoutDependency(result, layout.height, element, dependents);
    addLayoutDependency(result, layout.minWidth, element, dependents);
    addLayoutDependency(result, layout.minHeight, element, dependents);
    addLayoutDependency(result, layout.maxWidth, element, dependents);
    addLayoutDependency(result, layout.maxHeight, element, dependents);
    addLayoutDependency(result, layout.preferredWidth, element, dependents);
    addLayoutDependency(result, layout.preferredHeight, element, dependents);
    addLayoutDependency(result, layout.grow, element, dependents);
    addLayoutDependency(result, layout.padding.left, element, dependents);
    addLayoutDependency(result, layout.padding.top, element, dependents);
    addLayoutDependency(result, layout.padding.right, element, dependents);
    addLayoutDependency(result, layout.padding.bottom, element, dependents);
    addLayoutDependency(result, layout.spacing, element, dependents);
    addLayoutDependency(result, layout.counterSpacing, element, dependents);
    if (const auto* textVisual = std::get_if<TextVisual>(&element.visual))
      addLayoutDependency(result, textVisual->text, element, dependents);
    for (const auto& child : element.children)
      collectLayoutDependencies(child, result, dependents, parents, &element);
  }

  static bool sameRect(Rect a, Rect b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
  }
  void markMeasureDirty(PropertyId property) {
    const auto found = layoutDependents.find(property);
    if (found == layoutDependents.end()) return;
    for (const Element* element : found->second) {
      while (element) {
        const bool inserted = dirtyMeasureElements.insert(element).second;
        const auto parent = layoutParents.find(element);
        element = parent == layoutParents.end() ? nullptr : parent->second;
        if (!inserted) break;
      }
    }
  }

  template <typename Subject>
  [[nodiscard]] bool layoutCurrent(const Subject& subject, Rect viewport,
                                   float deviceScale) {
    const float normalizedScale = std::max(0.01f, deviceScale);
    if (layoutSubject != &subject ||
        layoutStructureGeneration != subject.layoutGeneration() ||
        !sameRect(layoutViewport, viewport) || layoutDeviceScale != normalizedScale ||
        layoutDependencies.size() != layoutObserved.size()) {
      fullMeasureInvalidation = true;
      changedLayoutDependencies.clear();
      dirtyMeasureElements.clear();
      return false;
    }
    const auto& properties = subject.properties();
    const auto generation = properties.generation();
    if (generation == layoutPropertyGeneration) return true;
    changedLayoutDependencies.clear();
    bool layoutChanged = false;
    for (std::size_t i = 0; i < layoutDependencies.size(); ++i) {
      if (properties.revision(layoutDependencies[i]) == layoutObserved[i]) continue;
      changedLayoutDependencies.push_back(i);
      markMeasureDirty(layoutDependencies[i]);
      layoutChanged = true;
    }
    if (layoutChanged) {
      fullMeasureInvalidation = false;
      return false;
    }
    layoutPropertyGeneration = generation;
    return true;
  }

  template <typename Subject>
  void rememberLayoutState(const Subject& subject, Rect viewport, float deviceScale) {
    const bool rebuildDependencies = layoutSubject != &subject ||
      layoutStructureGeneration != subject.layoutGeneration() || layoutDependencies.empty();
    layoutSubject = &subject;
    layoutStructureGeneration = subject.layoutGeneration();
    layoutViewport = viewport;
    layoutDeviceScale = std::max(0.01f, deviceScale);
    const auto& properties = subject.properties();
    if (rebuildDependencies) {
      layoutDependencies.clear();
      layoutDependents.clear();
      layoutParents.clear();
      collectLayoutDependencies(subject.root(), layoutDependencies,
                                layoutDependents, layoutParents);
      std::sort(layoutDependencies.begin(), layoutDependencies.end());
      layoutDependencies.erase(std::unique(layoutDependencies.begin(), layoutDependencies.end()),
                               layoutDependencies.end());
      layoutObserved.clear();
      layoutObserved.reserve(layoutDependencies.size());
      for (const auto property : layoutDependencies)
        layoutObserved.push_back(properties.revision(property));
    } else {
      for (const auto index : changedLayoutDependencies) {
        if (index < layoutObserved.size())
          layoutObserved[index] = properties.revision(layoutDependencies[index]);
      }
    }
    layoutPropertyGeneration = properties.generation();
    changedLayoutDependencies.clear();
    dirtyMeasureElements.clear();
    fullMeasureInvalidation = false;
  }

  [[nodiscard]] std::optional<float> length(const ValueSource<Length>& source,
                                             const PropertyStore& properties,
                                             float parent) const {
    const auto value = source.resolve(properties);
    switch (value.unit) {
      case LengthUnit::LogicalPixels: return value.value * scale;
      case LengthUnit::PhysicalPixels: return value.value;
      case LengthUnit::Percent: return parent * value.value * 0.01f;
      case LengthUnit::Auto: return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] float inset(const ValueSource<Length>& source,
                            const PropertyStore& properties, float parent) const {
    return std::max(0.0f, length(source, properties, parent).value_or(0.0f));
  }

  [[nodiscard]] Measured textSize(const TextVisual& textVisual,
                                  const PropertyStore& properties) const {
    const auto& value = textVisual.text.resolve(properties);
    const float glyphAdvance = textVisual.style.size * scale * 0.6f +
                               textVisual.style.letterSpacing * scale;
    const float lineHeight = textVisual.style.size * scale * textVisual.style.lineHeight;
    std::size_t maxCharacters = 0;
    std::size_t lines = 0;
    std::size_t start = 0;
    do {
      const auto end = value.find('\n', start);
      const auto line = std::string_view(value).substr(
        start, end == std::string::npos ? value.size() - start : end - start);
      maxCharacters = std::max(maxCharacters, decodeUtf8(line).size());
      ++lines;
      if (end == std::string::npos) break;
      start = end + 1;
    } while (start <= value.size());
    return {
      static_cast<float>(maxCharacters) * glyphAdvance +
        textVisual.style.indent * scale,
      static_cast<float>(lines) * lineHeight
    };
  }

  [[nodiscard]] Measured constrain(const Element& element, Measured measured,
                                   const PropertyStore& properties,
                                   float parentWidth, float parentHeight) const {
    const auto width = length(element.layout.width, properties, parentWidth);
    const auto height = length(element.layout.height, properties, parentHeight);
    const auto preferredWidth = length(element.layout.preferredWidth, properties, parentWidth);
    const auto preferredHeight = length(element.layout.preferredHeight, properties, parentHeight);
    if (width) measured.width = *width;
    else if (preferredWidth) measured.width = *preferredWidth;
    if (height) measured.height = *height;
    else if (preferredHeight) measured.height = *preferredHeight;

    if (const auto minimum = length(element.layout.minWidth, properties, parentWidth)) {
      measured.width = std::max(measured.width, *minimum);
    }
    if (const auto minimum = length(element.layout.minHeight, properties, parentHeight)) {
      measured.height = std::max(measured.height, *minimum);
    }
    if (const auto maximum = length(element.layout.maxWidth, properties, parentWidth)) {
      measured.width = std::min(measured.width, *maximum);
    }
    if (const auto maximum = length(element.layout.maxHeight, properties, parentHeight)) {
      measured.height = std::min(measured.height, *maximum);
    }
    measured.width = std::max(0.0f, measured.width);
    measured.height = std::max(0.0f, measured.height);
    return measured;
  }

  [[nodiscard]] Measured measure(const Element& element, const PropertyStore& properties,
                                 float parentWidth, float parentHeight) {
    const MeasureKey key{&element, std::bit_cast<std::uint32_t>(parentWidth),
                         std::bit_cast<std::uint32_t>(parentHeight)};
    if (const auto cached = measureCache.find(key); cached != measureCache.end()) {
      return cached->second;
    }
    ++stats.measureEvaluations;
    if (!element.visible.resolve(properties)) {
      measureCache.emplace(key, Measured{});
      return {};
    }
    const float left = inset(element.layout.padding.left, properties, parentWidth);
    const float right = inset(element.layout.padding.right, properties, parentWidth);
    const float top = inset(element.layout.padding.top, properties, parentHeight);
    const float bottom = inset(element.layout.padding.bottom, properties, parentHeight);
    const float innerWidth = std::max(0.0f, parentWidth - left - right);
    const float innerHeight = std::max(0.0f, parentHeight - top - bottom);
    // Figma auto-layout permits negative itemSpacing for overlapping children. Padding uses
    // inset()'s non-negative clamp, but spacing must preserve the authored sign.
    const float gap = length(
      element.layout.spacing, properties,
      element.layout.kind == LayoutKind::Row ? parentWidth : parentHeight).value_or(0.0f);

    Measured result = {};
    if (const auto* textVisual = std::get_if<TextVisual>(&element.visual)) {
      result = textSize(*textVisual, properties);
    }

    std::size_t visibleChildren = 0;
    float childrenWidth = 0.0f;
    float childrenHeight = 0.0f;
    if ((element.layout.kind == LayoutKind::Row || element.layout.kind == LayoutKind::Column) &&
        element.layout.wrap) {
      const bool horizontal = element.layout.kind == LayoutKind::Row;
      const float mainLimit = horizontal ? innerWidth : innerHeight;
      const float counterGap = length(
        element.layout.counterSpacing, properties,
        horizontal ? parentHeight : parentWidth).value_or(gap);
      float trackMain = 0.0f;
      float trackCross = 0.0f;
      float totalCross = 0.0f;
      float maximumMain = 0.0f;
      std::size_t trackItems = 0;
      std::size_t tracks = 0;
      const auto finishTrack = [&]() {
        if (trackItems == 0) return;
        maximumMain = std::max(maximumMain, trackMain);
        totalCross += trackCross;
        ++tracks;
        trackMain = 0.0f;
        trackCross = 0.0f;
        trackItems = 0;
      };
      for (const auto& child : element.children) {
        if (!child.visible.resolve(properties) || child.layout.absolutePositioned) continue;
        const auto childSize = measure(child, properties, innerWidth, innerHeight);
        const float childMain = horizontal ? childSize.width : childSize.height;
        const float childCross = horizontal ? childSize.height : childSize.width;
        const float candidate = trackItems == 0 ? childMain : trackMain + gap + childMain;
        if (trackItems != 0 && mainLimit > 0.0f && candidate > mainLimit + 0.0001f)
          finishTrack();
        if (trackItems != 0) trackMain += gap;
        trackMain += childMain;
        trackCross = std::max(trackCross, childCross);
        ++trackItems;
        ++visibleChildren;
      }
      finishTrack();
      if (tracks > 1) totalCross += counterGap * static_cast<float>(tracks - 1);
      if (horizontal) {
        childrenWidth = maximumMain;
        childrenHeight = totalCross;
      } else {
        childrenWidth = totalCross;
        childrenHeight = maximumMain;
      }
    } else {
      for (const auto& child : element.children) {
        if (!child.visible.resolve(properties)) continue;
        if ((element.layout.kind == LayoutKind::Row ||
             element.layout.kind == LayoutKind::Column) &&
            child.layout.absolutePositioned) {
          continue;
        }
        const auto childSize = measure(child, properties, innerWidth, innerHeight);
        ++visibleChildren;
        if (element.layout.kind == LayoutKind::Row) {
          childrenWidth += childSize.width;
          childrenHeight = std::max(childrenHeight, childSize.height);
        } else if (element.layout.kind == LayoutKind::Column) {
          childrenWidth = std::max(childrenWidth, childSize.width);
          childrenHeight += childSize.height;
        } else {
          const float x = length(child.layout.x, properties, innerWidth).value_or(0.0f);
          const float y = length(child.layout.y, properties, innerHeight).value_or(0.0f);
          childrenWidth = std::max(childrenWidth, x + childSize.width);
          childrenHeight = std::max(childrenHeight, y + childSize.height);
        }
      }
      if (visibleChildren > 1 &&
          (element.layout.kind == LayoutKind::Row ||
           element.layout.kind == LayoutKind::Column)) {
        const float spacing = gap * static_cast<float>(visibleChildren - 1);
        if (element.layout.kind == LayoutKind::Row) childrenWidth += spacing;
        else childrenHeight += spacing;
      }
    }
    result.width = std::max(result.width, childrenWidth + left + right);
    result.height = std::max(result.height, childrenHeight + top + bottom);
    const auto constrained = constrain(element, result, properties, parentWidth, parentHeight);
    measureCache.emplace(key, constrained);
    return constrained;
  }

  [[nodiscard]] float authoredExtent(const Element& element, bool horizontal,
                                     const PropertyStore& properties,
                                     float fallback) const {
    const auto& fixed = horizontal ? element.layout.width : element.layout.height;
    if (const auto value = length(fixed, properties, fallback))
      return std::max(0.0f, *value);
    const auto& preferred = horizontal
      ? element.layout.preferredWidth : element.layout.preferredHeight;
    if (const auto value = length(preferred, properties, fallback))
      return std::max(0.0f, *value);
    return std::max(0.0f, fallback);
  }

  [[nodiscard]] Rect constrainedChildBounds(
      const Element& parent, const Element& child, Rect parentBounds, Measured childSize,
      const PropertyStore& properties) const {
    const float sourceParentWidth = authoredExtent(parent, true, properties, parentBounds.width);
    const float sourceParentHeight = authoredExtent(parent, false, properties, parentBounds.height);
    const float sourceX = length(child.layout.x, properties, sourceParentWidth).value_or(0.0f);
    const float sourceY = length(child.layout.y, properties, sourceParentHeight).value_or(0.0f);

    const auto resolveAxis = [](Constraint constraint, float sourcePosition, float sourceSize,
                                float sourceParentSize, float actualParentSize) {
      float position = sourcePosition;
      float size = sourceSize;
      const float trailing = sourceParentSize - sourcePosition - sourceSize;
      switch (constraint) {
        case Constraint::Center:
          position = actualParentSize * 0.5f +
            (sourcePosition + sourceSize * 0.5f - sourceParentSize * 0.5f) -
            sourceSize * 0.5f;
          break;
        case Constraint::Max:
          position = actualParentSize - trailing - sourceSize;
          break;
        case Constraint::Stretch:
          position = sourcePosition;
          size = std::max(0.0f, actualParentSize - sourcePosition - trailing);
          break;
        case Constraint::Scale:
          if (sourceParentSize > 0.0001f) {
            const float ratio = actualParentSize / sourceParentSize;
            position = sourcePosition * ratio;
            size = sourceSize * ratio;
          }
          break;
        case Constraint::Min:
          break;
      }
      return std::pair{position, size};
    };

    const auto horizontal = resolveAxis(
      child.layout.horizontalConstraint, sourceX, childSize.width,
      sourceParentWidth, parentBounds.width);
    const auto vertical = resolveAxis(
      child.layout.verticalConstraint, sourceY, childSize.height,
      sourceParentHeight, parentBounds.height);
    return {
      parentBounds.x + horizontal.first,
      parentBounds.y + vertical.first,
      horizontal.second,
      vertical.second,
    };
  }

  void append(const Element& element, Rect bounds, Rect inheritedClip,
              const PropertyStore& properties, bool ancestorEnabled,
              bool ancestorOverlay, std::size_t depth) {
    ++stats.elements;
    if (!element.visible.resolve(properties)) return;
    ++stats.visibleElements;

    const bool enabled = ancestorEnabled && element.interaction.enabled.resolve(properties);
    const bool overlay = ancestorOverlay || element.overlay;
    const std::size_t resolvedPosition = resolved.size();
    const RuntimeNodeId node{currentInstanceId, element.id};
    resolved.push_back({
      {node, element.id, bounds, inheritedClip, overlay, element.interaction.interactive},
      &element,
      enabled
    });
    if (element.id != 0) resolvedIndex.try_emplace(node, resolvedPosition);

    const float left = inset(element.layout.padding.left, properties, bounds.width);
    const float right = inset(element.layout.padding.right, properties, bounds.width);
    const float top = inset(element.layout.padding.top, properties, bounds.height);
    const float bottom = inset(element.layout.padding.bottom, properties, bounds.height);
    Rect inner = {
      bounds.x + left,
      bounds.y + top,
      std::max(0.0f, bounds.width - left - right),
      std::max(0.0f, bounds.height - top - bottom)
    };
    const Rect childClip = element.clipsChildren ? intersect(inheritedClip, bounds) : inheritedClip;

    if (element.layout.kind == LayoutKind::Row ||
        element.layout.kind == LayoutKind::Column) {
      const bool horizontal = element.layout.kind == LayoutKind::Row;
      const float mainSize = horizontal ? inner.width : inner.height;
      const float crossSize = horizontal ? inner.height : inner.width;
      const float gap = length(element.layout.spacing, properties, mainSize).value_or(0.0f);
      auto& children = flowScratch[depth];
      children.clear();
      if (children.capacity() < element.children.size()) {
        children.reserve(element.children.size());
      }
      for (const auto& child : element.children) {
        if (!child.visible.resolve(properties) || child.layout.absolutePositioned) continue;
        auto childSize = measure(child, properties, inner.width, inner.height);
        const float grow = std::max(0.0f, child.layout.grow.resolve(properties));
        children.push_back({&child, childSize, grow});
      }

      struct Track {
        std::size_t begin = 0;
        std::size_t end = 0;
        float used = 0.0f;
        float cross = 0.0f;
        float grow = 0.0f;
      };
      std::vector<Track> tracks;
      if (!children.empty()) {
        Track track;
        track.begin = 0;
        for (std::size_t index = 0; index < children.size(); ++index) {
          auto& child = children[index];
          const float childMain = horizontal ? child.measured.width : child.measured.height;
          const float childCross = horizontal ? child.measured.height : child.measured.width;
          const float candidate = index == track.begin ? childMain : track.used + gap + childMain;
          if (element.layout.wrap && index != track.begin && mainSize > 0.0f &&
              candidate > mainSize + 0.0001f) {
            track.end = index;
            tracks.push_back(track);
            track = {};
            track.begin = index;
          }
          if (index != track.begin) track.used += gap;
          track.used += childMain;
          track.cross = std::max(track.cross, childCross);
          track.grow += child.grow;
        }
        track.end = children.size();
        tracks.push_back(track);
      }

      const float counterGap = element.layout.wrap
        ? length(element.layout.counterSpacing, properties, crossSize).value_or(gap)
        : 0.0f;
      float tracksCross = 0.0f;
      for (const auto& track : tracks)
        tracksCross += element.layout.wrap ? track.cross : crossSize;
      if (element.layout.wrap && tracks.size() > 1)
        tracksCross += counterGap * static_cast<float>(tracks.size() - 1);
      const float counterAvailable = std::max(0.0f, crossSize - tracksCross);
      float counterCursor = 0.0f;
      float actualCounterGap = counterGap;
      if (element.layout.wrap) {
        switch (element.layout.counterAlignment) {
          case Justify::Center: counterCursor = counterAvailable * 0.5f; break;
          case Justify::End: counterCursor = counterAvailable; break;
          case Justify::SpaceBetween:
            if (tracks.size() > 1)
              actualCounterGap += counterAvailable / static_cast<float>(tracks.size() - 1);
            break;
          case Justify::SpaceAround:
            if (!tracks.empty()) {
              const float distributed = counterAvailable / static_cast<float>(tracks.size());
              counterCursor = distributed * 0.5f;
              actualCounterGap += distributed;
            }
            break;
          case Justify::SpaceEvenly:
            if (!tracks.empty()) {
              const float distributed = counterAvailable / static_cast<float>(tracks.size() + 1);
              counterCursor = distributed;
              actualCounterGap += distributed;
            }
            break;
          case Justify::Start: break;
        }
      }

      for (auto& track : tracks) {
        const float available = std::max(0.0f, mainSize - track.used);
        if (track.grow > 0.0f) {
          for (std::size_t index = track.begin; index < track.end; ++index) {
            auto& child = children[index];
            const float addition = available * child.grow / track.grow;
            if (horizontal) child.measured.width += addition;
            else child.measured.height += addition;
          }
        }

        float cursor = 0.0f;
        float actualGap = gap;
        if (track.grow == 0.0f) {
          const auto itemCount = track.end - track.begin;
          switch (element.layout.mainAlignment) {
            case Justify::Center: cursor = available * 0.5f; break;
            case Justify::End: cursor = available; break;
            case Justify::SpaceBetween:
              if (itemCount > 1)
                actualGap += available / static_cast<float>(itemCount - 1);
              break;
            case Justify::SpaceAround:
              if (itemCount > 0) {
                const float distributed = available / static_cast<float>(itemCount);
                cursor = distributed * 0.5f;
                actualGap += distributed;
              }
              break;
            case Justify::SpaceEvenly:
              if (itemCount > 0) {
                const float distributed = available / static_cast<float>(itemCount + 1);
                cursor = distributed;
                actualGap += distributed;
              }
              break;
            case Justify::Start: break;
          }
        }

        const float trackCross = element.layout.wrap ? track.cross : crossSize;
        for (std::size_t index = track.begin; index < track.end; ++index) {
          auto& child = children[index];
          const auto alignment = child.element->layout.alignSelf.value_or(
            element.layout.crossAlignment);
          float childCross = horizontal ? child.measured.height : child.measured.width;
          const auto explicitCross = horizontal
            ? length(child.element->layout.height, properties, inner.height)
            : length(child.element->layout.width, properties, inner.width);
          if (alignment == Alignment::Stretch && !explicitCross) childCross = trackCross;
          float crossOffset = 0.0f;
          if (alignment == Alignment::Center) crossOffset = (trackCross - childCross) * 0.5f;
          else if (alignment == Alignment::End) crossOffset = trackCross - childCross;
          crossOffset = std::max(0.0f, crossOffset);

          if (horizontal) {
            child.bounds = {inner.x + cursor, inner.y + counterCursor + crossOffset,
                            child.measured.width, childCross};
            cursor += child.measured.width + actualGap;
          } else {
            child.bounds = {inner.x + counterCursor + crossOffset, inner.y + cursor,
                            childCross, child.measured.height};
            cursor += child.measured.height + actualGap;
          }
        }
        counterCursor += trackCross + actualCounterGap;
      }

      // Resolve flow first, then choose painter order independently from layout order.
      // Figma itemReverseZIndex=true means the first source child is on top, but its flow
      // position must not change.
      if (element.layout.reverseChildPaintOrder) {
        std::size_t flowIndex = children.size();
        for (auto iterator = element.children.rbegin(); iterator != element.children.rend(); ++iterator) {
          const auto& child = *iterator;
          if (!child.visible.resolve(properties)) continue;
          if (child.layout.absolutePositioned) {
            const auto childSize = measure(child, properties, bounds.width, bounds.height);
            append(child, constrainedChildBounds(element, child, bounds, childSize, properties),
                   childClip, properties, enabled, overlay, depth + 1);
            continue;
          }
          if (flowIndex == 0) continue;
          const auto childBounds = children[--flowIndex].bounds;
          append(child, childBounds, childClip, properties, enabled, overlay, depth + 1);
        }
      } else {
        std::size_t flowIndex = 0;
        for (const auto& child : element.children) {
          if (!child.visible.resolve(properties)) continue;
          if (child.layout.absolutePositioned) {
            const auto childSize = measure(child, properties, bounds.width, bounds.height);
            append(child, constrainedChildBounds(element, child, bounds, childSize, properties),
                   childClip, properties, enabled, overlay, depth + 1);
            continue;
          }
          if (flowIndex >= children.size()) continue;
          const auto childBounds = children[flowIndex++].bounds;
          append(child, childBounds, childClip, properties, enabled, overlay, depth + 1);
        }
      }
      return;
    }

    for (const auto& child : element.children) {
      if (!child.visible.resolve(properties)) {
        ++stats.elements;
        continue;
      }
      if (element.layout.kind == LayoutKind::Stack) {
        auto childSize = measure(child, properties, inner.width, inner.height);
        const float x = length(child.layout.x, properties, inner.width).value_or(0.0f);
        const float y = length(child.layout.y, properties, inner.height).value_or(0.0f);
        if (!length(child.layout.width, properties, inner.width)) childSize.width = inner.width;
        if (!length(child.layout.height, properties, inner.height)) childSize.height = inner.height;
        append(child, {inner.x + x, inner.y + y, childSize.width, childSize.height},
               childClip, properties, enabled, overlay, depth + 1);
      } else {
        const auto childSize = measure(child, properties, bounds.width, bounds.height);
        append(child, constrainedChildBounds(element, child, bounds, childSize, properties),
               childClip, properties, enabled, overlay, depth + 1);
      }
    }
  }

  [[nodiscard]] std::size_t treeDepth(const Element& element) const {
    std::size_t result = 1;
    for (const auto& child : element.children) {
      result = std::max(result, treeDepth(child) + 1);
    }
    return result;
  }

  static constexpr float hitCellSize = 64.0f;
  static constexpr std::size_t maximumIndexedCellsPerElement = 256;

  [[nodiscard]] static std::uint64_t hitCellKey(std::int32_t x, std::int32_t y) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
           static_cast<std::uint32_t>(y);
  }

  [[nodiscard]] static std::int32_t hitCellCoordinate(float value) noexcept {
    return static_cast<std::int32_t>(std::floor(value / hitCellSize));
  }

  void buildHitIndex() {
    hitCells.clear();
    hitGlobals.clear();
    hitCells.reserve(resolved.size());
    for (std::size_t index = 0; index < resolved.size(); ++index) {
      const auto& entry = resolved[index];
      const bool nestedInstance = std::holds_alternative<InstanceVisual>(entry.element->visual);
      if ((!entry.box.interactive && !nestedInstance) || !entry.enabled) continue;
      const Rect region = intersect(entry.box.bounds, entry.box.clip);
      if (region.width <= 0.0f || region.height <= 0.0f) continue;
      const auto minX = hitCellCoordinate(region.x);
      const auto minY = hitCellCoordinate(region.y);
      const auto maxX = hitCellCoordinate(std::nextafter(
        region.x + region.width, -std::numeric_limits<float>::infinity()));
      const auto maxY = hitCellCoordinate(std::nextafter(
        region.y + region.height, -std::numeric_limits<float>::infinity()));
      const std::uint64_t columns = static_cast<std::uint64_t>(maxX - minX + 1);
      const std::uint64_t rows = static_cast<std::uint64_t>(maxY - minY + 1);
      if (columns * rows > maximumIndexedCellsPerElement) {
        hitGlobals.push_back(index);
        continue;
      }
      for (auto y = minY; y <= maxY; ++y) {
        for (auto x = minX; x <= maxX; ++x) {
          hitCells[hitCellKey(x, y)].push_back(index);
        }
      }
    }
  }

  static InstanceId subjectInstanceId(const Component&) noexcept { return 0; }
  static InstanceId subjectInstanceId(const ComponentInstance& instance) noexcept {
    return instance.instanceId();
  }

  template <typename Subject>
  void performLayout(Subject& subject, Rect viewport, float deviceScale) {
    currentInstanceId = subjectInstanceId(subject);
    const std::size_t cacheLimit = std::max<std::size_t>(4096, resolved.size() * 8);
    if (fullMeasureInvalidation || measureCache.size() > cacheLimit) {
      measureCache.clear();
    } else if (!dirtyMeasureElements.empty()) {
      for (auto it = measureCache.begin(); it != measureCache.end();) {
        if (dirtyMeasureElements.contains(it->first.element)) it = measureCache.erase(it);
        else ++it;
      }
    }
    resolved.clear();
    publicBoxes.clear();
    resolvedIndex.clear();
    scale = std::max(0.01f, deviceScale);
    const auto& root = subject.root();
    const auto& properties = subject.properties();
    if (treeDepthSubject != &subject ||
        treeDepthStructureGeneration != subject.layoutGeneration()) {
      treeDepthSubject = &subject;
      treeDepthStructureGeneration = subject.layoutGeneration();
      cachedTreeDepth = treeDepth(root);
    }
    flowScratch.resize(cachedTreeDepth);
    auto rootSize = measure(root, properties, viewport.width, viewport.height);
    if (!length(root.layout.width, properties, viewport.width)) rootSize.width = viewport.width;
    if (!length(root.layout.height, properties, viewport.height)) rootSize.height = viewport.height;
    append(root, {viewport.x, viewport.y, rootSize.width, rootSize.height},
           viewport, properties, true, false, 0);
    publicBoxes.reserve(resolved.size());
    for (const auto& entry : resolved) publicBoxes.push_back(entry.box);
    buildHitIndex();
    rememberLayoutState(subject, viewport, deviceScale);
    ++stats.layoutPasses;
  }

  [[nodiscard]] const Resolved* resolvedById(RuntimeNodeId node) const {
    if (!node) return nullptr;
    const auto found = resolvedIndex.find(node);
    if (found == resolvedIndex.end() || found->second >= resolved.size()) return nullptr;
    return &resolved[found->second];
  }

  template <typename NestedHit>
  [[nodiscard]] RuntimeNodeId hitLayer(
      Vec2 cursor, HitTestLayer layer, bool inheritedOverlay,
      NestedHit&& nestedHit, RuntimeStats* hitStats = nullptr) const {
    const auto found = hitCells.find(hitCellKey(hitCellCoordinate(cursor.x),
                                               hitCellCoordinate(cursor.y)));
    const std::vector<std::size_t>* local = found == hitCells.end() ? nullptr : &found->second;
    std::size_t localCount = local ? local->size() : 0;
    std::size_t globalCount = hitGlobals.size();
    while (localCount != 0 || globalCount != 0) {
      const auto localIndex = localCount != 0 ? (*local)[localCount - 1] : 0;
      const auto globalIndex = globalCount != 0 ? hitGlobals[globalCount - 1] : 0;
      const bool takeLocal = globalCount == 0 ||
        (localCount != 0 && localIndex > globalIndex);
      const auto index = takeLocal ? (*local)[--localCount] : hitGlobals[--globalCount];
      if (hitStats) ++hitStats->hitCandidates;
      const auto& entry = resolved[index];
      const bool effectiveOverlay = inheritedOverlay || entry.box.overlay;
      if (layer == HitTestLayer::Overlay && !effectiveOverlay) continue;
      if (layer == HitTestLayer::Regular && effectiveOverlay) continue;
      if (!entry.box.bounds.contains(cursor) || !entry.box.clip.contains(cursor)) continue;

      if (const auto* nested = std::get_if<InstanceVisual>(&entry.element->visual)) {
        if (const auto nestedNode = nestedHit(entry, *nested, layer, effectiveOverlay)) {
          return nestedNode;
        }
      }
      if (entry.box.interactive && entry.enabled) return entry.box.node;
    }
    return {};
  }

  template <typename NestedHit>
  [[nodiscard]] RuntimeNodeId hitWithInstances(
      Vec2 cursor, HitTestLayer layer, bool inheritedOverlay,
      NestedHit&& nestedHit, RuntimeStats* hitStats = nullptr) const {
    if (layer == HitTestLayer::Any) {
      if (const auto overlay = hitLayer(
          cursor, HitTestLayer::Overlay, inheritedOverlay, nestedHit, hitStats)) {
        return overlay;
      }
      return hitLayer(
        cursor, HitTestLayer::Regular, inheritedOverlay, nestedHit, hitStats);
    }
    return hitLayer(cursor, layer, inheritedOverlay, nestedHit, hitStats);
  }

  [[nodiscard]] RuntimeNodeId hit(
      Vec2 cursor, HitTestLayer layer = HitTestLayer::Any,
      RuntimeStats* hitStats = nullptr) const {
    const auto noNested = [](const Resolved&, const InstanceVisual&,
                             HitTestLayer, bool) -> RuntimeNodeId { return {}; };
    return hitWithInstances(cursor, layer, false, noNested, hitStats);
  }

  template <typename Subject>
  void dispatch(Subject& subject, const Resolved* target, EventType type,
                const FrameInput& input) {
    if (!target || target->element->interaction.callback == 0) return;
    subject.dispatch(target->element->interaction.callback,
                     {type, target->box.id, target->box.node, input.cursor, input.cursorDelta});
    ++stats.callbacks;
  }

  template <typename Subject>
  void processInput(Subject& subject, const FrameInput& input) {
    const RuntimeNodeId nextHovered = hit(input.cursor, HitTestLayer::Any, &stats);
    if (nextHovered != hovered) {
      dispatch(subject, resolvedById(hovered), EventType::HoverExited, input);
      hovered = nextHovered;
      dispatch(subject, resolvedById(hovered), EventType::HoverEntered, input);
    }

    if (input.primary.pressed) {
      active = hovered;
      if (active) focused = active;
      const auto* target = resolvedById(active);
      dispatch(subject, target, EventType::Pressed, input);
      // Activation on the press edge keeps direct manipulation and buttons one frame behind
      // neither the OS cursor nor the current Vulkan submission.
      dispatch(subject, target, EventType::Activated, input);
    }
    if (input.primary.released) {
      dispatch(subject, resolvedById(active), EventType::Released, input);
      active = {};
    } else if (!input.primary.down && !input.primary.pressed) {
      active = {};
    }
  }

  [[nodiscard]] Paint paint(const StatefulPaint& state, const Resolved& entry,
                            const PropertyStore& properties,
                            const FrameInput& input) const {
    if (!entry.enabled && state.disabled) return state.disabled->resolve(properties);
    if (entry.box.node == active && input.primary.down && state.pressed) {
      return state.pressed->resolve(properties);
    }
    if (entry.box.node == hovered && state.hovered) return state.hovered->resolve(properties);
    return state.normal.resolve(properties);
  }

  void draw(const Resolved& entry, const PropertyStore& properties,
            const FrameInput& input, DrawList& drawList, Rect callerClip) const {
    drawList.setClip(intersect(entry.box.clip, callerClip));
    if (const auto* rounded = std::get_if<RoundedRectangleVisual>(&entry.element->visual)) {
      auto radii = rounded->radii.resolve(properties);
      radii.topLeft *= scale;
      radii.topRight *= scale;
      radii.bottomRight *= scale;
      radii.bottomLeft *= scale;
      BorderStyle border;
      border.paint = paint(rounded->stroke.paint, entry, properties, input);
      border.width = std::max(0.0f, rounded->stroke.width.resolve(properties) * scale);
      border.align = rounded->stroke.align;
      if (rounded->stroke.individualWidths) {
        auto widths = rounded->stroke.individualWidths->resolve(properties);
        widths.top = std::max(0.0f, widths.top * scale);
        widths.right = std::max(0.0f, widths.right * scale);
        widths.bottom = std::max(0.0f, widths.bottom * scale);
        widths.left = std::max(0.0f, widths.left * scale);
        border.individualWidths = widths;
      }
      const auto resolvedPaint = paint(rounded->paint, entry, properties, input);
      const auto smoothing = rounded->smoothing.resolve(properties);
      Rect imageBounds = entry.box.bounds;
      bool fittedImage = false;
      if (rounded->image.enabled && rounded->image.sourceWidth > 0.0f &&
          rounded->image.sourceHeight > 0.0f &&
          rounded->image.scaleMode == ImageScaleMode::Fit &&
          entry.box.bounds.width > 0.0f && entry.box.bounds.height > 0.0f) {
        const float fit = std::min(
          entry.box.bounds.width / rounded->image.sourceWidth,
          entry.box.bounds.height / rounded->image.sourceHeight);
        const float width = rounded->image.sourceWidth * fit;
        const float height = rounded->image.sourceHeight * fit;
        imageBounds = {
          entry.box.bounds.x + (entry.box.bounds.width - width) * 0.5f,
          entry.box.bounds.y + (entry.box.bounds.height - height) * 0.5f,
          width, height,
        };
        fittedImage = std::abs(width - entry.box.bounds.width) > 0.001f ||
                      std::abs(height - entry.box.bounds.height) > 0.001f;
      }

      if (fittedImage) {
        // FIT preserves the source aspect ratio inside the authored node rectangle. The image
        // placeholder itself has square inner edges; the node's own rounded clip/border remains
        // represented independently on the outer frame.
        drawList.roundedRect(imageBounds, 0.0f, resolvedPaint, 0.0f);
        bool hasBorder = border.width > 0.0001f;
        if (border.individualWidths) {
          const auto& widths = *border.individualWidths;
          hasBorder = hasBorder || widths.top > 0.0001f || widths.right > 0.0001f ||
                      widths.bottom > 0.0001f || widths.left > 0.0001f;
        }
        if (hasBorder) {
          drawList.roundedRect(
            entry.box.bounds, radii, Paint::solid(Color::fromRgb8(0x000000), 0.0f),
            smoothing, std::move(border));
        }
      } else {
        drawList.roundedRect(entry.box.bounds, radii, resolvedPaint,
                             smoothing, std::move(border));
      }
    } else if (const auto* shapeVisual = std::get_if<ShapeVisual>(&entry.element->visual)) {
      const auto placed = [&entry](const Rect& placement) {
        return Rect{
          entry.box.bounds.x + placement.x * entry.box.bounds.width,
          entry.box.bounds.y + placement.y * entry.box.bounds.height,
          placement.width * entry.box.bounds.width,
          placement.height * entry.box.bounds.height,
        };
      };
      if (shapeVisual->shape != 0) {
        drawList.shape(shapeVisual->shape, placed(shapeVisual->fillPlacement),
                       paint(shapeVisual->paint, entry, properties, input));
      }
      if (shapeVisual->strokeShape != 0) {
        drawList.shape(shapeVisual->strokeShape, placed(shapeVisual->strokePlacement),
                       paint(shapeVisual->strokePaint, entry, properties, input));
      }
    } else if (const auto* textVisual = std::get_if<TextVisual>(&entry.element->visual)) {
      auto style = textVisual->style;
      style.size *= scale;
      style.letterSpacing *= scale;
      style.indent *= scale;
      style.paint = paint(textVisual->paint, entry, properties, input);
      const auto& value = textVisual->text.resolve(properties);
      if (!textVisual->runs.empty()) {
        drawList.textRunsStatic(textVisual->runs, entry.box.bounds, std::move(style), scale);
      } else if (textVisual->text.bound()) {
        drawList.text(value, entry.box.bounds, std::move(style));
      } else {
        drawList.textStatic(value, entry.box.bounds, std::move(style));
      }
    }
  }

  template <typename InstanceEmitter>
  void emitWithInstances(
      const PropertyStore& properties, const FrameInput& input, DrawList& drawList,
      InstanceEmitter&& instanceEmitter) const {
    const Rect previousClip = drawList.clip();
    const bool previousOverlay = drawList.overlayMode();
    const auto emitEntry = [&](const Resolved& entry) {
      if (const auto* nested = std::get_if<InstanceVisual>(&entry.element->visual)) {
        drawList.setClip(intersect(entry.box.clip, previousClip));
        instanceEmitter(entry, *nested);
      } else {
        draw(entry, properties, input, drawList, previousClip);
      }
    };

    if (previousOverlay) {
      for (const auto& entry : resolved) emitEntry(entry);
    } else {
      for (const auto& entry : resolved) {
        if (!entry.box.overlay) emitEntry(entry);
      }
      drawList.beginOverlay();
      for (const auto& entry : resolved) {
        if (entry.box.overlay) emitEntry(entry);
      }
      drawList.endOverlay();
    }
    drawList.setClip(previousClip);
    if (previousOverlay) drawList.beginOverlay();
  }

  void emit(const PropertyStore& properties, const FrameInput& input, DrawList& drawList) const {
    const auto noNested = [](const Resolved&, const InstanceVisual&) {};
    emitWithInstances(properties, input, drawList, noNested);
  }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::layout(Component& component, Rect viewport, float deviceScale) {
  impl_->stats = {};
  component.properties().evaluateBindings();
  if (!impl_->layoutCurrent(component, viewport, deviceScale))
    impl_->performLayout(component, viewport, deviceScale);
}

void Runtime::layout(ComponentInstance& instance, Rect viewport, float deviceScale) {
  impl_->stats = {};
  instance.properties().evaluateBindings();
  if (!impl_->layoutCurrent(instance, viewport, deviceScale))
    impl_->performLayout(instance, viewport, deviceScale);
}

RuntimeStats Runtime::render(Component& component, const FrameInput& input,
                             DrawList& drawList, Rect viewport, float deviceScale) {
  impl_->stats = {};
  component.properties().evaluateBindings();
  if (!impl_->layoutCurrent(component, viewport, deviceScale))
    impl_->performLayout(component, viewport, deviceScale);
  impl_->processInput(component, input);
  component.properties().evaluateBindings();
  if (!impl_->layoutCurrent(component, viewport, deviceScale))
    impl_->performLayout(component, viewport, deviceScale);
  impl_->emit(component.properties(), input, drawList);
  return impl_->stats;
}

RuntimeStats Runtime::render(ComponentInstance& instance, const FrameInput& input,
                             DrawList& drawList, Rect viewport, float deviceScale) {
  impl_->stats = {};
  instance.properties().evaluateBindings();
  if (!impl_->layoutCurrent(instance, viewport, deviceScale))
    impl_->performLayout(instance, viewport, deviceScale);
  impl_->processInput(instance, input);
  instance.properties().evaluateBindings();
  if (!impl_->layoutCurrent(instance, viewport, deviceScale))
    impl_->performLayout(instance, viewport, deviceScale);
  impl_->emit(instance.properties(), input, drawList);
  return impl_->stats;
}

std::span<const LayoutBox> Runtime::boxes() const { return impl_->publicBoxes; }
RuntimeStats Runtime::stats() const { return impl_->stats; }
RuntimeNodeId Runtime::hitTest(Vec2 cursor, HitTestLayer layer) const {
  return impl_->hit(cursor, layer);
}

const LayoutBox* Runtime::find(ElementId id) const {
  return find(RuntimeNodeId{impl_->currentInstanceId, id});
}

const LayoutBox* Runtime::find(RuntimeNodeId node) const {
  if (!node) return nullptr;
  const auto found = impl_->resolvedIndex.find(node);
  if (found == impl_->resolvedIndex.end() || found->second >= impl_->publicBoxes.size()) return nullptr;
  return &impl_->publicBoxes[found->second];
}

RuntimeNodeId Runtime::hoveredNode() const { return impl_->hovered; }
RuntimeNodeId Runtime::activeNode() const { return impl_->active; }
RuntimeNodeId Runtime::focusedNode() const { return impl_->focused; }
ElementId Runtime::hovered() const { return impl_->hovered.element; }
ElementId Runtime::active() const { return impl_->active.element; }
ElementId Runtime::focused() const { return impl_->focused.element; }

class SceneRuntimeAccess {
public:
  struct NestedPlacement {
    const InstanceVisual* visual = nullptr;
    Rect bounds = {};
    Rect clip = {};
    bool overlay = false;
  };

  static void reset(Runtime& runtime) { runtime.impl_->stats = {}; }

  static RuntimeStats stats(const Runtime& runtime) { return runtime.impl_->stats; }

  static void ensureLayout(
      Runtime& runtime, ComponentInstance& instance, Rect viewport, float deviceScale) {
    instance.properties().evaluateBindings();
    if (!runtime.impl_->layoutCurrent(instance, viewport, deviceScale))
      runtime.impl_->performLayout(instance, viewport, deviceScale);
  }

  static void processInput(
      Runtime& runtime, ComponentInstance& instance, const FrameInput& input) {
    runtime.impl_->processInput(instance, input);
    instance.properties().evaluateBindings();
  }

  template <typename Function>
  static void forEachNested(const Runtime& runtime, Function&& function) {
    for (const auto& entry : runtime.impl_->resolved) {
      if (const auto* nested = std::get_if<InstanceVisual>(&entry.element->visual)) {
        function(NestedPlacement{nested, entry.box.bounds, entry.box.clip, entry.box.overlay});
      }
    }
  }

  template <typename NestedHit>
  static RuntimeNodeId hit(
      Runtime& runtime, Vec2 cursor, HitTestLayer layer, bool inheritedOverlay,
      NestedHit&& nestedHit) {
    return runtime.impl_->hitWithInstances(
      cursor, layer, inheritedOverlay,
      [&](const auto& entry, const InstanceVisual& nested,
          HitTestLayer childLayer, bool effectiveOverlay) {
        return nestedHit(
          NestedPlacement{&nested, entry.box.bounds, entry.box.clip, entry.box.overlay},
          childLayer, effectiveOverlay);
      },
      &runtime.impl_->stats);
  }

  template <typename NestedEmitter>
  static void emit(
      const Runtime& runtime, const PropertyStore& properties, const FrameInput& input,
      DrawList& drawList, NestedEmitter&& nestedEmitter) {
    runtime.impl_->emitWithInstances(
      properties, input, drawList,
      [&](const auto& entry, const InstanceVisual& nested) {
        nestedEmitter(
          NestedPlacement{&nested, entry.box.bounds, entry.box.clip, entry.box.overlay});
      });
  }
};

void Scene::add(ComponentInstance& instance, Rect viewport, float deviceScale) {
  if (instance.instanceId() == 0) {
    throw std::invalid_argument(
      "SlugUI Scene requires a non-zero stable ComponentInstance id");
  }
  if (viewport.width < 0.0f || viewport.height < 0.0f) {
    throw std::invalid_argument("SlugUI Scene viewport dimensions cannot be negative");
  }
  for (const auto& item : items_) {
    if (item.instance && item.instance->instanceId() == instance.instanceId()) {
      throw std::invalid_argument("SlugUI Scene instance ids must be unique");
    }
  }
  items_.push_back({&instance, viewport, std::max(0.01f, deviceScale)});
  ++generation_;
}

bool Scene::setViewport(InstanceId instance, Rect viewport, float deviceScale) {
  if (instance == 0) return false;
  if (viewport.width < 0.0f || viewport.height < 0.0f) {
    throw std::invalid_argument("SlugUI Scene viewport dimensions cannot be negative");
  }
  const float normalizedScale = std::max(0.01f, deviceScale);
  for (auto& item : items_) {
    if (!item.instance || item.instance->instanceId() != instance) continue;
    if (item.viewport.x == viewport.x && item.viewport.y == viewport.y &&
        item.viewport.width == viewport.width && item.viewport.height == viewport.height &&
        item.deviceScale == normalizedScale) {
      return false;
    }
    item.viewport = viewport;
    item.deviceScale = normalizedScale;
    ++generation_;
    return true;
  }
  return false;
}

bool Scene::remove(InstanceId instance) noexcept {
  for (auto iterator = items_.begin(); iterator != items_.end(); ++iterator) {
    if (!iterator->instance || iterator->instance->instanceId() != instance) continue;
    items_.erase(iterator);
    ++generation_;
    return true;
  }
  return false;
}

void Scene::clear() noexcept {
  if (items_.empty()) return;
  items_.clear();
  ++generation_;
}

struct SceneRuntime::Impl {
  struct State {
    ComponentInstance* instance = nullptr;
    std::unique_ptr<ComponentInstance> ownedInstance;
    Runtime runtime;
    InstanceId parentInstance = 0;
    InstanceId localInstance = 0;
    const ComponentDefinition* definition = nullptr;
    Rect viewport = {};
    float deviceScale = 1.0f;
  };
  struct Hit {
    RuntimeNodeId node = {};
    InstanceId instance = 0;
  };

  std::unordered_map<InstanceId, std::unique_ptr<State>> states;
  std::unordered_set<InstanceId> alive;
  const Scene* indexedScene = nullptr;
  std::uint64_t indexedGeneration = 0;
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> hitCells;
  std::vector<std::size_t> hitGlobals;
  RuntimeNodeId hovered = {};
  RuntimeNodeId active = {};
  RuntimeNodeId focused = {};

  static constexpr float sceneHitCellSize = 128.0f;
  static constexpr std::size_t maximumIndexedCellsPerInstance = 256;

  [[nodiscard]] static std::int32_t hitCellCoordinate(float value) noexcept {
    return static_cast<std::int32_t>(std::floor(value / sceneHitCellSize));
  }

  [[nodiscard]] static std::uint64_t hitCellKey(
      std::int32_t x, std::int32_t y) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
           static_cast<std::uint32_t>(y);
  }

  [[nodiscard]] State* stateFor(InstanceId id) {
    const auto found = states.find(id);
    return found == states.end() ? nullptr : found->second.get();
  }

  [[nodiscard]] const State* stateFor(InstanceId id) const {
    const auto found = states.find(id);
    return found == states.end() ? nullptr : found->second.get();
  }

  State& stateForTopLevel(const SceneItem& item) {
    const InstanceId id = item.instance->instanceId();
    if (auto* existing = stateFor(id)) {
      if (existing->ownedInstance || existing->instance != item.instance ||
          existing->parentInstance != 0 || existing->localInstance != 0) {
        throw std::logic_error("SlugUI Scene instance id collides with another retained instance");
      }
      existing->viewport = item.viewport;
      existing->deviceScale = item.deviceScale;
      alive.insert(id);
      return *existing;
    }

    auto state = std::make_unique<State>();
    state->instance = item.instance;
    state->definition = &item.instance->definition();
    state->viewport = item.viewport;
    state->deviceScale = item.deviceScale;
    auto* result = state.get();
    states.emplace(id, std::move(state));
    alive.insert(id);
    return *result;
  }

  static void syncNestedInputs(
      State& parent, State& child, const InstanceVisual& visual) {
    auto& properties = child.instance->properties();
    for (const auto& overrideValue : visual.overrides) {
      if (overrideValue.target == 0)
        throw std::invalid_argument("SlugUI nested instance override has invalid target property");
      properties.set(overrideValue.target, overrideValue.value);
    }
    for (const auto& link : visual.links) {
      if (link.source == 0 || link.target == 0)
        throw std::invalid_argument("SlugUI nested instance property link has invalid property id");
      properties.copy(link.target, parent.instance->properties(), link.source);
    }
  }

  State& stateForNested(State& parent, const InstanceVisual& visual) {
    if (!visual.definition)
      throw std::invalid_argument("SlugUI nested instance requires a definition");
    if (visual.localId == 0)
      throw std::invalid_argument("SlugUI nested instance local id must be non-zero");

    const InstanceId parentId = parent.instance->instanceId();
    const InstanceId id = composeInstanceId(parentId, visual.localId);
    if (id == parentId)
      throw std::logic_error("SlugUI nested instance id composition produced a parent collision");

    if (auto* existing = stateFor(id)) {
      if (!existing->ownedInstance ||
          existing->parentInstance != parentId ||
          existing->localInstance != visual.localId ||
          existing->definition != visual.definition.get()) {
        throw std::logic_error("SlugUI composed nested instance id collision");
      }
      syncNestedInputs(parent, *existing, visual);
      alive.insert(id);
      return *existing;
    }

    auto state = std::make_unique<State>();
    state->ownedInstance = std::make_unique<ComponentInstance>(visual.definition, id);
    state->instance = state->ownedInstance.get();
    state->parentInstance = parentId;
    state->localInstance = visual.localId;
    state->definition = visual.definition.get();
    syncNestedInputs(parent, *state, visual);
    auto* result = state.get();
    states.emplace(id, std::move(state));
    alive.insert(id);
    return *result;
  }

  void seedTopLevel(const Scene& scene) {
    alive.clear();
    alive.reserve(states.size() + scene.items().size());
    for (const auto& item : scene.items()) {
      if (!item.instance) continue;
      (void)stateForTopLevel(item);
    }
  }

  void beginFrame(const Scene& scene) {
    for (auto& [id, state] : states) SceneRuntimeAccess::reset(state->runtime);
    seedTopLevel(scene);
  }

  void ensureLayout(State& state) {
    SceneRuntimeAccess::ensureLayout(
      state.runtime, *state.instance, state.viewport, state.deviceScale);
  }

  void expandNested(
      State& state, std::vector<const ComponentDefinition*>& definitionStack,
      std::size_t depth = 0) {
    if (depth > 64)
      throw std::logic_error("SlugUI nested instance depth exceeded 64 levels");
    if (!state.definition)
      throw std::logic_error("SlugUI retained instance has no component definition");
    if (std::find(definitionStack.begin(), definitionStack.end(), state.definition) !=
        definitionStack.end()) {
      throw std::logic_error("SlugUI nested component definition cycle detected");
    }

    definitionStack.push_back(state.definition);
    ensureLayout(state);
    SceneRuntimeAccess::forEachNested(state.runtime, [&](const auto& placement) {
      if (!placement.visual) return;
      auto& child = stateForNested(state, *placement.visual);
      child.viewport = placement.bounds;
      child.deviceScale = state.deviceScale;
      expandNested(child, definitionStack, depth + 1);
    });
    definitionStack.pop_back();
  }

  void expandScene(const Scene& scene) {
    std::vector<const ComponentDefinition*> definitionStack;
    definitionStack.reserve(16);
    for (const auto& item : scene.items()) {
      if (!item.instance) continue;
      auto* state = stateFor(item.instance->instanceId());
      if (!state) continue;
      expandNested(*state, definitionStack);
    }
  }

  void pruneDeadStates() {
    for (auto iterator = states.begin(); iterator != states.end();) {
      if (!alive.contains(iterator->first)) iterator = states.erase(iterator);
      else ++iterator;
    }
    if (active && !alive.contains(active.instance)) active = {};
    if (hovered && !alive.contains(hovered.instance)) hovered = {};
    if (focused && !alive.contains(focused.instance)) focused = {};
  }

  void rebuildHitIndex(const Scene& scene) {
    if (indexedScene == &scene && indexedGeneration == scene.generation()) return;
    indexedScene = &scene;
    indexedGeneration = scene.generation();
    hitCells.clear();
    hitGlobals.clear();
    hitCells.reserve(scene.items().size());
    for (std::size_t index = 0; index < scene.items().size(); ++index) {
      const Rect region = scene.items()[index].viewport;
      if (region.width <= 0.0f || region.height <= 0.0f) continue;
      const auto minX = hitCellCoordinate(region.x);
      const auto minY = hitCellCoordinate(region.y);
      const auto maxX = hitCellCoordinate(std::nextafter(
        region.x + region.width, -std::numeric_limits<float>::infinity()));
      const auto maxY = hitCellCoordinate(std::nextafter(
        region.y + region.height, -std::numeric_limits<float>::infinity()));
      const std::uint64_t columns = static_cast<std::uint64_t>(maxX - minX + 1);
      const std::uint64_t rows = static_cast<std::uint64_t>(maxY - minY + 1);
      if (columns * rows > maximumIndexedCellsPerInstance) {
        hitGlobals.push_back(index);
        continue;
      }
      for (auto y = minY; y <= maxY; ++y) {
        for (auto x = minX; x <= maxX; ++x) {
          hitCells[hitCellKey(x, y)].push_back(index);
        }
      }
    }
  }

  [[nodiscard]] RuntimeNodeId hitState(
      State& state, Vec2 cursor, HitTestLayer layer, bool inheritedOverlay) {
    return SceneRuntimeAccess::hit(
      state.runtime, cursor, layer, inheritedOverlay,
      [&](const auto& placement, HitTestLayer childLayer, bool effectiveOverlay) {
        if (!placement.visual) return RuntimeNodeId{};
        const InstanceId childId = composeInstanceId(
          state.instance->instanceId(), placement.visual->localId);
        auto* child = stateFor(childId);
        if (!child || !alive.contains(childId)) return RuntimeNodeId{};
        return hitState(*child, cursor, childLayer, effectiveOverlay);
      });
  }

  [[nodiscard]] Hit hit(const Scene& scene, Vec2 cursor, HitTestLayer layer,
                        SceneRuntimeStats& stats) {
    const auto found = hitCells.find(hitCellKey(
      hitCellCoordinate(cursor.x), hitCellCoordinate(cursor.y)));
    const std::vector<std::size_t>* local =
      found == hitCells.end() ? nullptr : &found->second;
    std::size_t localCount = local ? local->size() : 0;
    std::size_t globalCount = hitGlobals.size();
    while (localCount != 0 || globalCount != 0) {
      const auto localIndex = localCount != 0 ? (*local)[localCount - 1] : 0;
      const auto globalIndex = globalCount != 0 ? hitGlobals[globalCount - 1] : 0;
      const bool takeLocal = globalCount == 0 ||
        (localCount != 0 && localIndex > globalIndex);
      const auto index = takeLocal ? (*local)[--localCount] : hitGlobals[--globalCount];
      if (index >= scene.items().size()) continue;
      ++stats.instanceCandidates;
      const auto& item = scene.items()[index];
      if (!item.instance || !item.viewport.contains(cursor)) continue;
      auto* state = stateFor(item.instance->instanceId());
      if (!state) continue;
      if (const auto node = hitState(*state, cursor, layer, false)) {
        return {node, node.instance};
      }
    }
    return {};
  }

  [[nodiscard]] static FrameInput routedInput(
      const FrameInput& input, InstanceId target, InstanceId current) {
    if (target == current) return input;
    constexpr float offscreen = -1000000000.0f;
    FrameInput routed = input;
    routed.cursor = {offscreen, offscreen};
    routed.cursorDelta = {};
    routed.primary = {};
    return routed;
  }

  void processInputs(InstanceId target, const FrameInput& input) {
    for (auto& [id, state] : states) {
      if (!alive.contains(id)) continue;
      const auto routed = routedInput(input, target, id);
      SceneRuntimeAccess::processInput(state->runtime, *state->instance, routed);
    }
  }

  void emitState(
      State& state, InstanceId target, const FrameInput& input, DrawList& drawList) {
    const auto routed = routedInput(input, target, state.instance->instanceId());
    SceneRuntimeAccess::emit(
      state.runtime, state.instance->properties(), routed, drawList,
      [&](const auto& placement) {
        if (!placement.visual) return;
        const InstanceId childId = composeInstanceId(
          state.instance->instanceId(), placement.visual->localId);
        auto* child = stateFor(childId);
        if (!child || !alive.contains(childId)) return;
        emitState(*child, target, input, drawList);
      });
  }

  static void accumulate(SceneRuntimeStats& result, RuntimeStats stats) {
    result.layoutPasses += stats.layoutPasses;
    result.measureEvaluations += stats.measureEvaluations;
    result.callbacks += stats.callbacks;
    result.hitCandidates += stats.hitCandidates;
  }
};

SceneRuntime::SceneRuntime() : impl_(std::make_unique<Impl>()) {}
SceneRuntime::~SceneRuntime() = default;
SceneRuntime::SceneRuntime(SceneRuntime&&) noexcept = default;
SceneRuntime& SceneRuntime::operator=(SceneRuntime&&) noexcept = default;

SceneRuntimeStats SceneRuntime::render(
    Scene& scene, const FrameInput& input, DrawList& drawList) {
  SceneRuntimeStats result;
  impl_->beginFrame(scene);
  impl_->rebuildHitIndex(scene);
  impl_->expandScene(scene);

  Impl::Hit target;
  const bool captured = impl_->active &&
    (input.primary.down || input.primary.released);
  if (captured && impl_->alive.contains(impl_->active.instance) &&
      impl_->stateFor(impl_->active.instance)) {
    target = {impl_->active, impl_->active.instance};
  } else {
    target = impl_->hit(scene, input.cursor, HitTestLayer::Overlay, result);
    if (!target.node)
      target = impl_->hit(scene, input.cursor, HitTestLayer::Regular, result);
  }

  if (input.primary.pressed) {
    impl_->active = target.node;
    if (target.node) impl_->focused = target.node;
  }

  impl_->processInputs(target.instance, input);

  if (target.instance != 0) {
    if (const auto* state = impl_->stateFor(target.instance)) {
      impl_->hovered = state->runtime.hoveredNode();
      if (input.primary.pressed && state->runtime.focusedNode())
        impl_->focused = state->runtime.focusedNode();
      if (state->runtime.activeNode())
        impl_->active = state->runtime.activeNode();
    }
  } else {
    impl_->hovered = {};
  }

  if (input.primary.released || (!input.primary.down && !input.primary.pressed))
    impl_->active = {};

  // Callbacks may have changed parent layout/property inputs. Re-seed the live tree, propagate
  // parent->child links, and relayout only the affected retained runtimes before lowering.
  impl_->seedTopLevel(scene);
  impl_->expandScene(scene);

  for (const auto& item : scene.items()) {
    if (!item.instance) continue;
    auto* state = impl_->stateFor(item.instance->instanceId());
    if (!state) continue;
    impl_->emitState(*state, target.instance, input, drawList);
  }

  result.instances = static_cast<std::uint32_t>(impl_->alive.size());
  for (const auto& [id, state] : impl_->states) {
    if (!impl_->alive.contains(id)) continue;
    Impl::accumulate(result, SceneRuntimeAccess::stats(state->runtime));
  }
  impl_->pruneDeadStates();
  return result;
}

const LayoutBox* SceneRuntime::find(RuntimeNodeId node) const {
  if (!node) return nullptr;
  const auto* state = impl_->stateFor(node.instance);
  return state ? state->runtime.find(node) : nullptr;
}

ComponentInstance* SceneRuntime::instance(InstanceId id) noexcept {
  auto* state = impl_->stateFor(id);
  return state ? state->instance : nullptr;
}

const ComponentInstance* SceneRuntime::instance(InstanceId id) const noexcept {
  const auto* state = impl_->stateFor(id);
  return state ? state->instance : nullptr;
}

RuntimeNodeId SceneRuntime::hoveredNode() const { return impl_->hovered; }
RuntimeNodeId SceneRuntime::activeNode() const { return impl_->active; }
RuntimeNodeId SceneRuntime::focusedNode() const { return impl_->focused; }

} // namespace slugia::slugui
