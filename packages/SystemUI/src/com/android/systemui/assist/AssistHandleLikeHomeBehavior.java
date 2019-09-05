/*
 * Copyright (C) 2019 The Android Open Source Project
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

package com.android.systemui.assist;

import android.content.Context;

import androidx.annotation.Nullable;

import com.android.systemui.assist.AssistHandleBehaviorController.BehaviorController;
import com.android.systemui.keyguard.WakefulnessLifecycle;
import com.android.systemui.recents.OverviewProxyService;
import com.android.systemui.shared.system.QuickStepContract;

import java.io.PrintWriter;

import javax.inject.Inject;
import javax.inject.Singleton;

import dagger.Lazy;

/**
 * Assistant Handle behavior that makes Assistant handles show/hide when the home handle is
 * shown/hidden, respectively.
 */
@Singleton
final class AssistHandleLikeHomeBehavior implements BehaviorController {

    private final WakefulnessLifecycle.Observer mWakefulnessLifecycleObserver =
            new WakefulnessLifecycle.Observer() {
                @Override
                public void onStartedWakingUp() {
                    handleDozingChanged(/* isDozing = */ true);
                }

                @Override
                public void onFinishedWakingUp() {
                    handleDozingChanged(/* isDozing = */ false);
                }

                @Override
                public void onStartedGoingToSleep() {
                    handleDozingChanged(/* isDozing = */ true);
                }

                @Override
                public void onFinishedGoingToSleep() {
                    handleDozingChanged(/* isDozing = */ true);
                }
            };
    private final OverviewProxyService.OverviewProxyListener mOverviewProxyListener =
            new OverviewProxyService.OverviewProxyListener() {
        @Override
        public void onSystemUiStateChanged(int sysuiStateFlags) {
            handleSystemUiStateChange(sysuiStateFlags);
        }
    };


    private final Lazy<WakefulnessLifecycle> mWakefulnessLifecycle;
    private final Lazy<OverviewProxyService> mOverviewProxyService;

    private boolean mIsDozing;
    private boolean mIsHomeHandleHiding;

    @Nullable private AssistHandleCallbacks mAssistHandleCallbacks;

    @Inject
    AssistHandleLikeHomeBehavior(
            Lazy<WakefulnessLifecycle> wakefulnessLifecycle,
            Lazy<OverviewProxyService> overviewProxyService) {
        mWakefulnessLifecycle = wakefulnessLifecycle;
        mOverviewProxyService = overviewProxyService;
    }

    @Override
    public void onModeActivated(Context context, AssistHandleCallbacks callbacks) {
        mAssistHandleCallbacks = callbacks;
        mIsDozing = mWakefulnessLifecycle.get().getWakefulness()
                != WakefulnessLifecycle.WAKEFULNESS_AWAKE;
        mWakefulnessLifecycle.get().addObserver(mWakefulnessLifecycleObserver);
        mOverviewProxyService.get().addCallback(mOverviewProxyListener);
        callbackForCurrentState();
    }

    @Override
    public void onModeDeactivated() {
        mAssistHandleCallbacks = null;
        mWakefulnessLifecycle.get().removeObserver(mWakefulnessLifecycleObserver);
        mOverviewProxyService.get().removeCallback(mOverviewProxyListener);
    }

    private static boolean isHomeHandleHiding(int sysuiStateFlags) {
        return (sysuiStateFlags & QuickStepContract.SYSUI_STATE_NAV_BAR_HIDDEN) != 0;
    }

    private void handleDozingChanged(boolean isDozing) {
        if (mIsDozing == isDozing) {
            return;
        }

        mIsDozing = isDozing;
        callbackForCurrentState();
    }

    private void handleSystemUiStateChange(int sysuiStateFlags) {
        boolean isHomeHandleHiding = isHomeHandleHiding(sysuiStateFlags);
        if (mIsHomeHandleHiding == isHomeHandleHiding) {
            return;
        }

        mIsHomeHandleHiding = isHomeHandleHiding;
        callbackForCurrentState();
    }

    private void callbackForCurrentState() {
        if (mAssistHandleCallbacks == null) {
            return;
        }

        if (mIsHomeHandleHiding || mIsDozing) {
            mAssistHandleCallbacks.hide();
        } else {
            mAssistHandleCallbacks.showAndStay();
        }
    }

    @Override
    public void dump(PrintWriter pw, String prefix) {
        pw.println("Current AssistHandleLikeHomeBehavior State:");

        pw.println(prefix + "   mIsDozing=" + mIsDozing);
        pw.println(prefix + "   mIsHomeHandleHiding=" + mIsHomeHandleHiding);
    }
}
