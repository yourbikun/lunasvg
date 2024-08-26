#include "svgtextelement.h"
#include "svglayoutstate.h"
#include "svgrenderstate.h"

#include <cassert>

namespace lunasvg {

inline const SVGTextPositioningElement* SVGTextFragment::element() const
{
    assert(position.node && position.node->isTextNode());
    auto parent = position.node->parent();
    assert(parent && parent->isTextPositioningElement());
    return static_cast<const SVGTextPositioningElement*>(parent);
}

static float calculateBaselineOffset(const SVGTextPositioningElement* element)
{
    return 0.f;
}

static bool needsTextAnchorAdjustment(const SVGTextPositioningElement* element)
{
    auto direction = element->direction();
    switch (element->text_anchor()) {
    case TextAnchor::Start:
        return direction == Direction::Rtl;
    case TextAnchor::Middle:
        return true;
    case TextAnchor::End:
        return direction == Direction::Ltr;
    default:
        assert(false);
    }

    return false;
}

static float calculateTextAnchorOffset(const SVGTextPositioningElement* element, float width)
{
    auto direction = element->direction();
    switch (element->text_anchor()) {
    case TextAnchor::Start:
        if(direction == Direction::Ltr)
            return 0.f;
        return -width;
    case TextAnchor::Middle:
        return -width / 2.f;
    case TextAnchor::End:
        if(direction == Direction::Ltr)
            return -width;
        return 0.f;
    default:
        assert(false);
    }

    return 0.f;
}

void SVGTextFragmentsBuilder::build(const SVGTextElement* element)
{
    handleElement(element);
    for(const auto& position : m_textPositions) {
        fillCharacterPositions(position);
    }

    for(const auto& textPosition : m_textPositions) {
        if(!textPosition.node->isTextNode())
            continue;
        auto parent = textPosition.node->parent();
        assert(parent && parent->isTextPositioningElement());
        auto element = static_cast<const SVGTextPositioningElement*>(parent);
        SVGTextFragment fragment(textPosition);
        auto recordTextFragment = [&](auto startOffset, auto endOffset) {
            auto text = std::u32string_view(m_text).substr(startOffset, endOffset - startOffset);
            fragment.position.startOffset = startOffset;
            fragment.position.endOffset = endOffset;
            fragment.width = element->font().measureText(text);
            m_fragments.push_back(fragment);
            m_x += fragment.width;
        };

        auto baselineOffset = calculateBaselineOffset(element);
        auto startOffset = textPosition.startOffset;
        auto textOffset = textPosition.startOffset;
        auto didStartTextFragment = false;
        auto lastAngle = 0.f;
        while(textOffset < textPosition.endOffset) {
            SVGCharacterPosition characterPosition;
            if(m_characterPositions.count(m_characterOffset) > 0) {
                characterPosition = m_characterPositions.at(m_characterOffset);
            }

            auto angle = characterPosition.rotate.value_or(0);
            auto dx = characterPosition.dx.value_or(0);
            auto dy = characterPosition.dy.value_or(0);

            auto shouldStartNewFragment = characterPosition.x || characterPosition.y || dx || dy || angle || angle != lastAngle;
            if(shouldStartNewFragment && didStartTextFragment) {
                recordTextFragment(startOffset, textOffset);
                startOffset = textOffset;
            }

            auto startsNewTextChunk = (characterPosition.x || characterPosition.y) && textOffset == textPosition.startOffset;
            if(startsNewTextChunk || shouldStartNewFragment || !didStartTextFragment) {
                m_x = dx + characterPosition.x.value_or(m_x);
                m_y = dy + characterPosition.y.value_or(m_y);
                fragment.startsNewTextChunk = startsNewTextChunk;
                fragment.x = m_x;
                fragment.y = m_y - baselineOffset;
                fragment.angle = angle;
                didStartTextFragment = true;
            }

            lastAngle = angle;
            ++textOffset;
            ++m_characterOffset;
        }

        recordTextFragment(startOffset, textOffset);
    }

    auto handleTextChunk = [](auto begin, auto end) {
        auto element = begin->element();
        if(!needsTextAnchorAdjustment(element))
            return;
        float chunkWidth = 0.f;
        const SVGTextFragment* lastFragment = nullptr;
        for(auto it = begin; it != end; ++it) {
            const SVGTextFragment& fragment = *it;
            chunkWidth += fragment.width;
            if(lastFragment)
                chunkWidth += fragment.x - (lastFragment->x + lastFragment->width);
            lastFragment = &fragment;
        }

        auto chunkOffset = calculateTextAnchorOffset(element, chunkWidth);
        for(auto it = begin; it != end; ++it) {
            SVGTextFragment& fragment = *it;
            fragment.x += chunkOffset;
        }
    };

    if(m_fragments.empty())
        return;
    auto it = m_fragments.begin();
    auto begin = m_fragments.begin();
    auto end = m_fragments.end();
    for(++it; it != end; ++it) {
        const SVGTextFragment& fragment = *it;
        if(!fragment.startsNewTextChunk)
            continue;
        handleTextChunk(begin, it);
        begin = it;
    }

    handleTextChunk(begin, it);
}

void SVGTextFragmentsBuilder::handleText(const SVGTextNode* node)
{
    const auto& text = node->text();
    if(text.empty())
        return;
    auto element = static_cast<const SVGTextPositioningElement*>(node->parent());
    const auto startOffset = m_text.length();
    uint32_t lastCharacter = ' ';
    if(!m_text.empty()) {
        lastCharacter = m_text.back();
    }

    plutovg_text_iterator_t it;
    plutovg_text_iterator_init(&it, text.data(), text.length(), PLUTOVG_TEXT_ENCODING_UTF8);
    while(plutovg_text_iterator_has_next(&it)) {
        auto currentCharacter = plutovg_text_iterator_next(&it);
        if(currentCharacter == '\t' || currentCharacter == '\n' || currentCharacter == '\r')
            currentCharacter = ' ';
        if(element->white_space() == WhiteSpace::Default && currentCharacter == ' ' && lastCharacter == ' ')
            continue;
        m_text.push_back(currentCharacter);
        lastCharacter = currentCharacter;
    }

    m_textPositions.emplace_back(node, startOffset, m_text.length());
}

void SVGTextFragmentsBuilder::handleElement(const SVGTextPositioningElement* element)
{
    auto itemIndex = m_textPositions.size();
    m_textPositions.emplace_back(element, m_text.length(), m_text.length());
    for(const auto& child : element->children()) {
        if(child->isTextNode()) {
            handleText(static_cast<SVGTextNode*>(child.get()));
        } else if(child->isTextPositioningElement()) {
            handleElement(static_cast<SVGTextPositioningElement*>(child.get()));
        }
    }

    auto& position = m_textPositions[itemIndex];
    assert(position.node == element);
    position.endOffset = m_text.length();
}

void SVGTextFragmentsBuilder::fillCharacterPositions(const SVGTextPosition& position)
{
    if(!position.node->isTextPositioningElement())
        return;
    auto element = static_cast<const SVGTextPositioningElement*>(position.node);
    const auto& xList = element->x();
    const auto& yList = element->y();
    const auto& dxList = element->dx();
    const auto& dyList = element->dy();
    const auto& rotateList = element->rotate();

    auto xListSize = xList.size();
    auto yListSize = yList.size();
    auto dxListSize = dxList.size();
    auto dyListSize = dyList.size();
    auto rotateListSize = rotateList.size();
    if(!xListSize && !yListSize && !dxListSize && !dyListSize && !rotateListSize) {
        return;
    }

    LengthContext lengthContext(element);
    std::optional<float> lastRotation;
    for(auto offset = position.startOffset; offset < position.endOffset; ++offset) {
        auto index = offset - position.startOffset;
        if(index >= xListSize && index >= yListSize && index >= dxListSize && index >= dyListSize && index >= rotateListSize)
            break;
        auto& characterPosition = m_characterPositions[offset];
        if(index < xListSize)
            characterPosition.x = lengthContext.valueForLength(xList[index], LengthDirection::Horizontal);
        if(index < yListSize)
            characterPosition.y = lengthContext.valueForLength(yList[index], LengthDirection::Vertical);
        if(index < dxListSize)
            characterPosition.dx = lengthContext.valueForLength(dxList[index], LengthDirection::Horizontal);
        if(index < dyListSize)
            characterPosition.dy = lengthContext.valueForLength(dyList[index], LengthDirection::Vertical);
        if(index < rotateListSize) {
            characterPosition.rotate = rotateList[index];
            lastRotation = characterPosition.rotate;
        }
    }

    if(lastRotation == std::nullopt)
        return;
    auto offset = position.startOffset + rotateList.size();
    while(offset < position.endOffset) {
        m_characterPositions[offset++].rotate = lastRotation;
    }
}

SVGTextPositioningElement::SVGTextPositioningElement(Document* document, ElementID id)
    : SVGGraphicsElement(document, id)
    , m_x(PropertyID::X, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_y(PropertyID::Y, LengthDirection::Vertical, LengthNegativeMode::Allow)
    , m_dx(PropertyID::Dx, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_dy(PropertyID::Dy, LengthDirection::Vertical, LengthNegativeMode::Allow)
    , m_rotate(PropertyID::Rotate)
{
    addProperty(m_x);
    addProperty(m_y);
    addProperty(m_dx);
    addProperty(m_dy);
    addProperty(m_rotate);
}

void SVGTextPositioningElement::layoutElement(const SVGLayoutState& state)
{
    m_font = state.font();
    m_fill = getPaintServer(state.fill(), state.fill_opacity());
    m_stroke = getPaintServer(state.stroke(), state.stroke_opacity());

    LengthContext lengthContext(this);
    m_stroke_width = lengthContext.valueForLength(state.stroke_width(), LengthDirection::Diagonal);
    m_text_anchor = state.text_anchor();
    m_white_space = state.white_space();
    SVGGraphicsElement::layoutElement(state);
}

SVGTSpanElement::SVGTSpanElement(Document* document)
    : SVGTextPositioningElement(document, ElementID::Tspan)
{
}

SVGTextElement::SVGTextElement(Document* document)
    : SVGTextPositioningElement(document, ElementID::Text)
{
}

void SVGTextElement::layout(SVGLayoutState& state)
{
    SVGTextPositioningElement::layout(state);
    SVGTextFragmentsBuilder(m_text, m_fragments).build(this);

    m_fillBoundingBox = Rect::Invalid;
    for(const auto& fragment : m_fragments) {
        auto element = fragment.element();
        auto fragmentRect = Rect(fragment.x, fragment.y - element->font().ascent(), fragment.width, element->font().height());
        auto fragmentTranform = Transform::translated(fragment.x, fragment.y);
        fragmentTranform.rotate(fragment.angle);
        fragmentTranform.translate(-fragment.x, -fragment.y);
        m_fillBoundingBox.unite(fragmentTranform.mapRect(fragmentRect));
    }

    if(!m_fillBoundingBox.isValid()) {
        m_fillBoundingBox = Rect::Empty;
    }
}

void SVGTextElement::render(SVGRenderState& state) const
{
    if(isVisibilityHidden() || isDisplayNone())
        return;
    SVGBlendInfo blendInfo(this);
    SVGRenderState newState(this, state, localTransform());
    newState.beginGroup(blendInfo);
    if(newState.mode() == SVGRenderMode::Clipping) {
        newState->setColor(Color::White);
    }

    std::u32string_view textView(m_text);
    for(const auto& fragment : m_fragments) {
        auto text = textView.substr(fragment.position.startOffset, fragment.position.endOffset - fragment.position.startOffset);
        auto transform = newState.currentTransform();
        Point origin(fragment.x, fragment.y);
        transform.translate(origin.x, origin.y);
        transform.rotate(fragment.angle);
        transform.translate(-origin.x, -origin.y);

        auto element = fragment.element();
        if(state.mode() == SVGRenderMode::Clipping) {
            newState->fillText(text, element->font(), origin, transform);
        } else {
            if(element->fill().applyPaint(newState)) {
                newState->fillText(text, element->font(), origin, transform);
            } else if(element->stroke().applyPaint(newState)) {
                newState->strokeText(text, element->stroke_width(), element->font(), origin, transform);
            }
        }
    }

    newState.endGroup(blendInfo);
}

} // namespace lunasvg