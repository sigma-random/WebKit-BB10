/*
 * Copyright (C) 2010, 2011, 2012, 2013 Research In Motion Limited. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "TouchEventHandler.h"

#include "BlackBerryPlatformSystemSound.h"
#include "DOMSupport.h"
#include "Document.h"
#include "DocumentMarkerController.h"
#include "FocusController.h"
#include "Frame.h"
#include "FrameView.h"
#include "HTMLAnchorElement.h"
#include "HTMLAreaElement.h"
#include "HTMLImageElement.h"
#include "HTMLInputElement.h"
#include "HTMLNames.h"
#include "HTMLPlugInElement.h"
#include "InRegionScroller_p.h"
#include "InputHandler.h"
#include "IntRect.h"
#include "IntSize.h"
#include "Node.h"
#include "Page.h"
#include "PlatformMouseEvent.h"
#include "PlatformTouchEvent.h"
#include "RenderLayer.h"
#include "RenderTheme.h"
#include "RenderView.h"
#include "SelectionHandler.h"
#include "WebKitThreadViewportAccessor.h"
#include "WebPage_p.h"
#include "WebTapHighlight.h"

#include <BlackBerryPlatformViewportAccessor.h>
#include <wtf/MathExtras.h>

using namespace WebCore;
using namespace WTF;

namespace BlackBerry {
namespace WebKit {

TouchEventHandler::TouchEventHandler(WebPagePrivate* webpage)
    : m_webPage(webpage)
    , m_shouldRequestSpellCheckOptions(false)
    , m_userTriggeredTouchPressed(true)
    , m_touchPressed(false)
    , m_mouseClickDelayed(false)
    , m_mouseOverElement(0)
    , m_visibilityChange(ChromeClient::NoVisibilityChange)
    , m_timersPotentiallyCausingVisibilityChange(0)
{
}

TouchEventHandler::~TouchEventHandler()
{
}

void TouchEventHandler::doFatFingers(const Platform::TouchPoint& point)
{
    m_lastScreenPoint = point.screenPosition();
    m_lastFatFingersResult.reset(); // Theoretically this shouldn't be required. Keep it just in case states get mangled.
    m_webPage->postponeDocumentStyleRecalc();
    m_lastFatFingersResult = FatFingers(m_webPage, point.documentContentPosition(), FatFingers::ClickableElement).findBestPoint();
    m_webPage->resumeDocumentStyleRecalc();
}

void TouchEventHandler::sendClickAtFatFingersPoint()
{
    handleFatFingerPressed();
    handleFatFingerReleased();
}

void TouchEventHandler::handleTouchHold()
{
    // Clear and reset focus if the user touch-holds on a different frame.
    // Special case for highlighting text on a frame not currently under focus [PR 285211].
    Node* nodeUnderFatFinger = m_lastFatFingersResult.node();
    if (nodeUnderFatFinger && nodeUnderFatFinger->document()->frame() != m_webPage->focusedOrMainFrame()) {
        m_webPage->clearFocusNode();
        m_webPage->m_selectionHandler->cancelSelection();
        m_webPage->m_page->focusController()->setFocusedFrame(nodeUnderFatFinger->document()->frame());
    }
}

void TouchEventHandler::handleTouchPoint(const Platform::TouchPoint& point)
{
    // Enable input mode on any touch event.
    m_webPage->m_inputHandler->setInputModeEnabled();

    switch (point.state()) {
    case Platform::TouchPoint::TouchPressed:
        {
            // Reset HoverHack Variables
            m_touchPressed = true;
            m_mouseOverElement = 0;
            m_visibilityChange = ChromeClient::NoVisibilityChange;

            // Clear spellcheck state on any touch event
            m_webPage->m_inputHandler->clearDidSpellCheckState();

            if (!m_lastFatFingersResult.isValid())
                doFatFingers(point);

            // Check for text selection
            if (m_lastFatFingersResult.isTextInput()) {
                Element* elementUnderFatFinger = m_lastFatFingersResult.nodeAsElementIfApplicable(FatFingersResult::ShadowContentNotAllowed, true /* shouldUseRootEditableElement */);
                m_shouldRequestSpellCheckOptions = m_webPage->m_inputHandler->shouldRequestSpellCheckingOptionsForPoint(m_lastFatFingersResult.adjustedPosition(), elementUnderFatFinger, m_spellCheckOptionRequest);
            }

            m_webPage->m_inputHandler->elementTouched(lastFatFingersResult().nodeAsElementIfApplicable(FatFingersResult::ShadowContentNotAllowed));

            handleFatFingerPressed();
            break;
        }
    case Platform::TouchPoint::TouchReleased:
        {
            if (!m_shouldRequestSpellCheckOptions)
                m_webPage->m_inputHandler->processPendingKeyboardVisibilityChange();

            // The rebase has eliminated a necessary event when the mouse does not
            // trigger an actual selection change preventing re-showing of the
            // keyboard. If input mode is active, call showVirtualKeyboard which
            // will update the state and display keyboard if needed.
            if (m_webPage->m_inputHandler->isInputMode())
                m_webPage->m_inputHandler->notifyClientOfKeyboardVisibilityChange(true);

            m_webPage->m_tapHighlight->hide();
            m_webPage->m_selectionHighlight->hide();

            handleFatFingerReleased();

            if (m_shouldRequestSpellCheckOptions) {
                m_webPage->m_inputHandler->requestSpellCheckingOptions(m_spellCheckOptionRequest);
                m_shouldRequestSpellCheckOptions = false;
            }

            // HoverHack: Don't reset the fat fingers if we're waiting for the JS timers to return a potential visibility change
            if (m_visibilityChange != ChromeClient::PotentialVisibilityChange)
                m_lastFatFingersResult.reset(); // Reset the fat finger result as its no longer valid when a user's finger is not on the screen.
            m_touchPressed = false;
            break;
        }
    case Platform::TouchPoint::TouchMoved:
        {
            // Clear spellcheck state on any touch event
            m_webPage->m_inputHandler->clearDidSpellCheckState();

            // You can still send mouse move events
            PlatformMouseEvent mouseEvent(point.documentViewportPosition(), m_lastScreenPoint, PlatformEvent::MouseMoved, 1, LeftButton, false /* shiftActive */, false /* ctrlActive */, false /* altActive */, TouchScreen);
            m_lastScreenPoint = point.screenPosition();
            m_webPage->handleMouseEvent(mouseEvent);
            break;
        }
    default:
        break;
    }
}

void TouchEventHandler::handleFatFingerPressed()
{
    const Platform::ViewportAccessor* viewportAccessor = m_webPage->m_webkitThreadViewportAccessor;
    IntPoint documentViewportAdjustedPosition = viewportAccessor->documentViewportFromContents(m_lastFatFingersResult.adjustedPosition());

    if (m_visibilityChange == ChromeClient::NoVisibilityChange) {
        m_timersPotentiallyCausingVisibilityChange = 0;
        // First update the mouse position with a MouseMoved event.
        PlatformMouseEvent mouseMoveEvent(documentViewportAdjustedPosition, m_lastScreenPoint, PlatformEvent::MouseMoved, 0, LeftButton, false /* shiftActive */, false /* ctrlActive */, false /* altActive */, TouchScreen);
        m_webPage->handleMouseEvent(mouseMoveEvent);
    }

    // If the visibility of something has changed due to the mouse move or a JS timer has started, delay sending the click
    if (m_mouseOverElement && m_visibilityChange != ChromeClient::NoVisibilityChange && !m_mouseClickDelayed) {
        if (m_visibilityChange == ChromeClient::VisibilityChange) {
            m_visibilityChange = ChromeClient::NoVisibilityChange;
            m_mouseClickDelayed = true;
        }
        return;
    }

    m_mouseClickDelayed = false;
    m_mouseOverElement = 0;

    // Then send the MousePressed event.
    PlatformMouseEvent mousePressedEvent(documentViewportAdjustedPosition, m_lastScreenPoint, PlatformEvent::MousePressed, 1, LeftButton, false /* shiftActive */, false /* ctrlActive */, false /* altActive */, TouchScreen);
    m_webPage->handleMouseEvent(mousePressedEvent);
}

void TouchEventHandler::handleFatFingerReleased()
{
    if (m_mouseClickDelayed || m_visibilityChange == ChromeClient::PotentialVisibilityChange)
        return;

    const Platform::ViewportAccessor* viewportAccessor = m_webPage->m_webkitThreadViewportAccessor;
    IntPoint documentViewportAdjustedPosition = viewportAccessor->documentViewportFromContents(m_lastFatFingersResult.adjustedPosition());
    PlatformMouseEvent mouseEvent(documentViewportAdjustedPosition, m_lastScreenPoint, PlatformEvent::MouseReleased, 1, LeftButton, false /* shiftActive */, false /* ctrlActive */, false /* altActive */, TouchScreen);

    m_webPage->handleMouseEvent(mouseEvent);
}

// This method filters what element will get tap-highlight'ed or not. To start with,
// we are going to highlight links (anchors with a valid href element), and elements
// whose tap highlight color value is different than the default value.
static Element* elementForTapHighlight(Element* elementUnderFatFinger)
{
    // Do not bail out right way here if there element does not have a renderer.
    // It is the casefor <map> (descendent of <area>) elements. The associated <image>
    // element actually has the renderer.
    if (elementUnderFatFinger->renderer()) {
        Color tapHighlightColor = elementUnderFatFinger->renderStyle()->tapHighlightColor();
        if (tapHighlightColor != RenderTheme::defaultTheme()->platformTapHighlightColor())
            return elementUnderFatFinger;
    }

    bool isArea = elementUnderFatFinger->hasTagName(HTMLNames::areaTag);
    Node* linkNode = elementUnderFatFinger->isLink() ? elementUnderFatFinger : 0;
    if (!linkNode || !linkNode->isHTMLElement() || (!linkNode->renderer() && !isArea))
        return 0;

    ASSERT(linkNode->isLink());

    // FatFingers class selector ensure only anchor with valid href attr value get here.
    // It includes empty hrefs.
    Element* highlightCandidateElement = toElement(linkNode);

    if (!isArea)
        return highlightCandidateElement;

    HTMLAreaElement* area = static_cast<HTMLAreaElement*>(highlightCandidateElement);
    HTMLImageElement* image = area->imageElement();
    if (image && image->renderer())
        return image;

    return 0;
}

void TouchEventHandler::drawTapHighlight()
{
    Element* elementUnderFatFinger = m_lastFatFingersResult.nodeAsElementIfApplicable();
    if (!elementUnderFatFinger)
        return;

    Element* element = elementForTapHighlight(elementUnderFatFinger);
    if (!element)
        return;

    // Get the element bounding rect in transformed coordinates so we can extract
    // the focus ring relative position each rect.
    RenderObject* renderer = element->renderer();
    ASSERT(renderer);

    Frame* elementFrame = element->document()->frame();
    ASSERT(elementFrame);

    FrameView* elementFrameView = elementFrame->view();
    if (!elementFrameView)
        return;

    // Tell the client if the element is either in a scrollable container or in a fixed positioned container.
    // On the client side, this info is being used to hide the tap highlight window on scroll.
    RenderLayer* layer = m_webPage->enclosingFixedPositionedAncestorOrSelfIfFixedPositioned(renderer->enclosingLayer());
    bool shouldHideTapHighlightRightAfterScrolling = !layer->renderer()->isRenderView();
    shouldHideTapHighlightRightAfterScrolling |= !!m_webPage->m_inRegionScroller->d->isActive();

    IntPoint framePos(m_webPage->frameOffset(elementFrame));

    // FIXME: We can get more precise on the <map> case by calculating the rect with HTMLAreaElement::computeRect().
    IntRect absoluteRect = renderer->absoluteBoundingBoxRect(true);
    absoluteRect.move(framePos.x(), framePos.y());

    IntRect clippingRect;
    if (elementFrame == m_webPage->mainFrame())
        clippingRect = IntRect(IntPoint(0, 0), elementFrameView->contentsSize());
    else
        clippingRect = m_webPage->mainFrame()->view()->windowToContents(m_webPage->getRecursiveVisibleWindowRect(elementFrameView, true /*noClipToMainFrame*/));
    clippingRect = intersection(absoluteRect, clippingRect);

    Vector<FloatQuad> focusRingQuads;
    renderer->absoluteFocusRingQuads(focusRingQuads);

    Platform::IntRectRegion region;
    for (size_t i = 0; i < focusRingQuads.size(); ++i) {
        IntRect rect = focusRingQuads[i].enclosingBoundingBox();
        rect.move(framePos.x(), framePos.y());
        IntRect clippedRect = intersection(clippingRect, rect);
        // FIXME we shouldn't have any empty rects here PR 246960
        if (clippedRect.isEmpty())
            continue;
        clippedRect.inflate(2);
        region = unionRegions(region, Platform::IntRect(clippedRect));
    }

    Color highlightColor = element->renderStyle()->tapHighlightColor();

    m_webPage->m_tapHighlight->draw(region,
        highlightColor.red(), highlightColor.green(), highlightColor.blue(), highlightColor.alpha(),
        shouldHideTapHighlightRightAfterScrolling);
}

void TouchEventHandler::playSoundIfAnchorIsTarget() const
{
    if (m_lastFatFingersResult.node() && m_lastFatFingersResult.node()->isLink())
        BlackBerry::Platform::SystemSound::instance()->playSound(BlackBerry::Platform::SystemSoundType::InputKeypress);
}

void TouchEventHandler::notifyVisibilityChange(ChromeClient::VisibilityChangeType change)
{
    switch (change) {
    case ChromeClient::VisibilityChange:
        if (m_visibilityChange == ChromeClient::PotentialVisibilityChange)
            m_mouseClickDelayed = true;
        if (m_touchPressed || m_visibilityChange == ChromeClient::PotentialVisibilityChange)
            m_visibilityChange = change;
        break;
    case ChromeClient::PotentialVisibilityChange:
        if (m_visibilityChange != ChromeClient::VisibilityChange && m_touchPressed && m_mouseOverElement) {
            m_timersPotentiallyCausingVisibilityChange++;
            m_visibilityChange = change;
        }
        break;
    case ChromeClient::PotentialVisibilityChangeEnded:
        if (m_visibilityChange != ChromeClient::PotentialVisibilityChange)
            break;
        m_timersPotentiallyCausingVisibilityChange--;
        if (m_timersPotentiallyCausingVisibilityChange)
            break;
        m_visibilityChange = ChromeClient::NoVisibilityChange;
        if (m_mouseOverElement) {
            handleFatFingerPressed();
            handleFatFingerReleased();
        }
        break;
    case ChromeClient::NoVisibilityChange:
        m_visibilityChange = change;
        break;
    }
}

void TouchEventHandler::mouseDidMoveOverElement(const HitTestResult& result, unsigned flags)
{
    UNUSED_PARAM(flags);

    if (!m_touchPressed)
        return;

    Element* element = result.innerElement();
    // If its a new element reset the hover hack mouse click
    if (element && m_mouseOverElement != element && m_mouseClickDelayed)
        m_mouseClickDelayed = false;

    m_mouseOverElement = element;
}

} // namespace WebKit
} // namespace BlackBerry
