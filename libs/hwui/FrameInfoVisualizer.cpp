/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "FrameInfoVisualizer.h"

#include "BakedOpRenderer.h"
#include "utils/Color.h"

#include <cutils/compiler.h>
#include <array>

#define RETURN_IF_PROFILING_DISABLED() if (CC_LIKELY(mType == ProfileType::None)) return
#define RETURN_IF_DISABLED() if (CC_LIKELY(mType == ProfileType::None && !mShowDirtyRegions)) return

#define PROFILE_DRAW_WIDTH 3
#define PROFILE_DRAW_THRESHOLD_STROKE_WIDTH 2
#define PROFILE_DRAW_DP_PER_MS 7

namespace android {
namespace uirenderer {

// Must be NUM_ELEMENTS in size
static const SkColor THRESHOLD_COLOR = Color::Green_500;
static const SkColor BAR_FAST_MASK = 0x8FFFFFFF;
static const SkColor BAR_JANKY_MASK = 0xDFFFFFFF;

// We could get this from TimeLord and use the actual frame interval, but
// this is good enough
#define FRAME_THRESHOLD 16
#define FRAME_THRESHOLD_NS 16000000

struct BarSegment {
    FrameInfoIndex start;
    FrameInfoIndex end;
    SkColor color;
};

static const std::array<BarSegment,7> Bar {{
    { FrameInfoIndex::IntendedVsync, FrameInfoIndex::HandleInputStart, Color::Teal_700 },
    { FrameInfoIndex::HandleInputStart, FrameInfoIndex::PerformTraversalsStart, Color::Green_700 },
    { FrameInfoIndex::PerformTraversalsStart, FrameInfoIndex::DrawStart, Color::LightGreen_700 },
    { FrameInfoIndex::DrawStart, FrameInfoIndex::SyncStart, Color::Blue_500 },
    { FrameInfoIndex::SyncStart, FrameInfoIndex::IssueDrawCommandsStart, Color::LightBlue_300 },
    { FrameInfoIndex::IssueDrawCommandsStart, FrameInfoIndex::SwapBuffers, Color::Red_500},
    { FrameInfoIndex::SwapBuffers, FrameInfoIndex::FrameCompleted, Color::Orange_500},
}};

static int dpToPx(int dp, float density) {
    return (int) (dp * density + 0.5f);
}

FrameInfoVisualizer::FrameInfoVisualizer(FrameInfoSource& source)
        : mFrameSource(source) {
    setDensity(1);
}

FrameInfoVisualizer::~FrameInfoVisualizer() {
    destroyData();
}

void FrameInfoVisualizer::setDensity(float density) {
    if (CC_UNLIKELY(mDensity != density)) {
        mDensity = density;
        mVerticalUnit = dpToPx(PROFILE_DRAW_DP_PER_MS, density);
        mThresholdStroke = dpToPx(PROFILE_DRAW_THRESHOLD_STROKE_WIDTH, density);
    }
}

void FrameInfoVisualizer::unionDirty(SkRect* dirty) {
    RETURN_IF_DISABLED();
    // Not worth worrying about minimizing the dirty region for debugging, so just
    // dirty the entire viewport.
    if (dirty) {
        mDirtyRegion = *dirty;
        dirty->setEmpty();
    }
}

void FrameInfoVisualizer::draw(ContentRenderer* renderer) {
    RETURN_IF_DISABLED();

    if (mShowDirtyRegions) {
        mFlashToggle = !mFlashToggle;
        if (mFlashToggle) {
            SkPaint paint;
            paint.setColor(0x7fff0000);
            renderer->drawRect(mDirtyRegion.fLeft, mDirtyRegion.fTop,
                    mDirtyRegion.fRight, mDirtyRegion.fBottom, &paint);
        }
    }

    if (mType == ProfileType::Bars) {
        // Patch up the current frame to pretend we ended here. CanvasContext
        // will overwrite these values with the real ones after we return.
        // This is a bit nicer looking than the vague green bar, as we have
        // valid data for almost all the stages and a very good idea of what
        // the issue stage will look like, too
        FrameInfo& info = mFrameSource.back();
        info.markSwapBuffers();
        info.markFrameCompleted();

        initializeRects(renderer->getViewportHeight(), renderer->getViewportWidth());
        drawGraph(renderer);
        drawThreshold(renderer);
    }
}

void FrameInfoVisualizer::createData() {
    if (mFastRects.get()) return;

    mFastRects.reset(new float[mFrameSource.capacity() * 4]);
    mJankyRects.reset(new float[mFrameSource.capacity() * 4]);
}

void FrameInfoVisualizer::destroyData() {
    mFastRects.reset(nullptr);
    mJankyRects.reset(nullptr);
}

void FrameInfoVisualizer::initializeRects(const int baseline, const int width) {
    // Target the 95% mark for the current frame
    float right = width * .95;
    float baseLineWidth = right / mFrameSource.capacity();
    mNumFastRects = 0;
    mNumJankyRects = 0;
    int fast_i = 0, janky_i = 0;
    // Set the bottom of all the shapes to the baseline
    for (int fi = mFrameSource.size() - 1; fi >= 0; fi--) {
        if (mFrameSource[fi][FrameInfoIndex::Flags] & FrameInfoFlags::SkippedFrame) {
            continue;
        }
        float lineWidth = baseLineWidth;
        float* rect;
        int ri;
        // Rects are LTRB
        if (mFrameSource[fi].totalDuration() <= FRAME_THRESHOLD_NS) {
            rect = mFastRects.get();
            ri = fast_i;
            fast_i += 4;
            mNumFastRects++;
        } else {
            rect = mJankyRects.get();
            ri = janky_i;
            janky_i += 4;
            mNumJankyRects++;
            lineWidth *= 2;
        }

        rect[ri + 0] = right - lineWidth;
        rect[ri + 1] = baseline;
        rect[ri + 2] = right;
        rect[ri + 3] = baseline;
        right -= lineWidth;
    }
}

void FrameInfoVisualizer::nextBarSegment(FrameInfoIndex start, FrameInfoIndex end) {
    int fast_i = (mNumFastRects - 1) * 4;
    int janky_i = (mNumJankyRects - 1) * 4;;
    for (size_t fi = 0; fi < mFrameSource.size(); fi++) {
        if (mFrameSource[fi][FrameInfoIndex::Flags] & FrameInfoFlags::SkippedFrame) {
            continue;
        }

        float* rect;
        int ri;
        // Rects are LTRB
        if (mFrameSource[fi].totalDuration() <= FRAME_THRESHOLD_NS) {
            rect = mFastRects.get();
            ri = fast_i;
            fast_i -= 4;
        } else {
            rect = mJankyRects.get();
            ri = janky_i;
            janky_i -= 4;
        }

        // Set the bottom to the old top (build upwards)
        rect[ri + 3] = rect[ri + 1];
        // Move the top up by the duration
        rect[ri + 1] -= mVerticalUnit * durationMS(fi, start, end);
    }
}

void FrameInfoVisualizer::drawGraph(ContentRenderer* renderer) {
    SkPaint paint;
    for (size_t i = 0; i < Bar.size(); i++) {
        nextBarSegment(Bar[i].start, Bar[i].end);
        paint.setColor(Bar[i].color & BAR_FAST_MASK);
        renderer->drawRects(mFastRects.get(), mNumFastRects * 4, &paint);
        paint.setColor(Bar[i].color & BAR_JANKY_MASK);
        renderer->drawRects(mJankyRects.get(), mNumJankyRects * 4, &paint);
    }
}

void FrameInfoVisualizer::drawThreshold(ContentRenderer* renderer) {
    SkPaint paint;
    paint.setColor(THRESHOLD_COLOR);
    float yLocation = renderer->getViewportHeight() - (FRAME_THRESHOLD * mVerticalUnit);
    renderer->drawRect(0.0f,
            yLocation - mThresholdStroke/2,
            renderer->getViewportWidth(),
            yLocation + mThresholdStroke/2,
            &paint);
}

bool FrameInfoVisualizer::consumeProperties() {
    bool changed = false;
    ProfileType newType = Properties::getProfileType();
    if (newType != mType) {
        mType = newType;
        if (mType == ProfileType::None) {
            destroyData();
        } else {
            createData();
        }
        changed = true;
    }

    bool showDirty = Properties::showDirtyRegions;
    if (showDirty != mShowDirtyRegions) {
        mShowDirtyRegions = showDirty;
        changed = true;
    }
    return changed;
}

void FrameInfoVisualizer::dumpData(int fd) {
    RETURN_IF_PROFILING_DISABLED();

    // This method logs the last N frames (where N is <= mDataSize) since the
    // last call to dumpData(). In other words if there's a dumpData(), draw frame,
    // dumpData(), the last dumpData() should only log 1 frame.

    FILE *file = fdopen(fd, "a");
    fprintf(file, "\n\tDraw\tPrepare\tProcess\tExecute\n");

    for (size_t i = 0; i < mFrameSource.size(); i++) {
        if (mFrameSource[i][FrameInfoIndex::IntendedVsync] <= mLastFrameLogged) {
            continue;
        }
        mLastFrameLogged = mFrameSource[i][FrameInfoIndex::IntendedVsync];
        fprintf(file, "\t%3.2f\t%3.2f\t%3.2f\t%3.2f\n",
                durationMS(i, FrameInfoIndex::IntendedVsync, FrameInfoIndex::SyncStart),
                durationMS(i, FrameInfoIndex::SyncStart, FrameInfoIndex::IssueDrawCommandsStart),
                durationMS(i, FrameInfoIndex::IssueDrawCommandsStart, FrameInfoIndex::SwapBuffers),
                durationMS(i, FrameInfoIndex::SwapBuffers, FrameInfoIndex::FrameCompleted));
    }

    fflush(file);
}

} /* namespace uirenderer */
} /* namespace android */
