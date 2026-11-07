#include "WaywallenDisplay.hpp"

#include "QsgPresentationNode.hpp"
#include "RenderSessionResources.hpp"

#include <waywallen_display.h>

#include <QMatrix4x4>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPointer>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QtQuick/qsgtexture_platform.h>
#ifdef WW_HAVE_VULKAN
#    include <vulkan/vulkan.h>
#endif
#include <cerrno>
#include <unistd.h>

// ---------------------------------------------------------------------------
// EGL: deferred GL texture creation (called on render thread)
// ---------------------------------------------------------------------------

void WaywallenDisplay::ensureGlTextures() {
    auto* display = displayHandle();
    if (m_glTexturesCreated || ! m_eglImagesValid || ! display) return;

    m_glTextures.resize(static_cast<int>(m_textureCount));
    bool ok = true;
    for (uint32_t i = 0; i < m_textureCount; i++) {
        uint32_t tex = 0;
        int      rc  = waywallen_display_create_gl_texture(display, i, &tex);
        if (rc != WAYWALLEN_OK) {
            qCWarning(lcWD, "create_gl_texture[%u] failed: %d", i, rc);
            ok = false;
            break;
        }
        m_glTextures[static_cast<int>(i)] = tex;
    }

    if (ok) {
        m_glTexturesCreated = true;
        qCInfo(lcWD, "created %u GL textures on render thread", m_textureCount);
    } else {
        m_glTextures.clear();
    }
}

WaywallenDisplay::EglBlitResult WaywallenDisplay::blitEglShadow(int slot, int width, int height,
                                                                bool forceReplace,
                                                                bool reuseCandidate) {
    auto resourcesOwner = renderSessionResources();
    if (! resourcesOwner || slot < 0 || slot >= m_glTextures.size()) {
        return EglBlitResult::Failed;
    }
    auto& resources = *resourcesOwner;
    auto* ctx       = QOpenGLContext::currentContext();
    if (! ctx) return EglBlitResult::Failed;
    auto* gl = ctx->extraFunctions();
    if (! gl || ! resources.ensureEglPresenter(gl) || width <= 0 || height <= 0) {
        return EglBlitResult::Failed;
    }

    const waywallen_egl_prepare_result_t result =
        waywallen_egl_presenter_prepare(resources.eglPresenter,
                                        m_glTextures[slot],
                                        static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height),
                                        forceReplace,
                                        reuseCandidate);
    switch (result) {
    case WAYWALLEN_EGL_PREPARE_CURRENT_UPDATED: return EglBlitResult::CurrentUpdated;
    case WAYWALLEN_EGL_PREPARE_CANDIDATE_READY: {
        if (! reuseCandidate) {
            waywallen_egl_presentable_descriptor_t current {};
            waywallen_egl_presentable_descriptor_t candidate {};
            (void)waywallen_egl_presenter_current(resources.eglPresenter, &current);
            (void)waywallen_egl_presenter_candidate(resources.eglPresenter, &candidate);
            qCInfo(lcWD,
                   "EGL shadow candidate: display=%llu %dx%d bytes=%llu resident=%llu",
                   m_displayId,
                   width,
                   height,
                   qulonglong(candidate.allocation_size),
                   qulonglong(current.allocation_size + candidate.allocation_size));
        }
        return EglBlitResult::CandidateReady;
    }
    case WAYWALLEN_EGL_PREPARE_FAILED_AFTER_GPU_WORK: return EglBlitResult::FailedAfterGpuWork;
    case WAYWALLEN_EGL_PREPARE_FAILED: return EglBlitResult::Failed;
    }
    return EglBlitResult::Failed;
}

// Drains m_pendingEgl and runs blitEglShadow. Scheduled by
// c_on_frame_ready via scheduleRenderJob(BeforeSynchronizingStage),
// so the imported buffer gets copied to the shadow exactly once per
// frame_ready arrival — Qt repaints driven by other dirty sources
// don't trigger a redundant blit.
void WaywallenDisplay::renderThreadBlitEgl() {
    PendingEglFrame frame;
    ContentSnapshot incoming;
    bool            replacesPresentation = false;
    bool            reuseCandidate       = false;
    {
        QMutexLocker lk(&m_pendingMutex);
        if (! m_pendingEgl.valid) return;
        frame        = m_pendingEgl;
        m_pendingEgl = PendingEglFrame {};
        (void)waywallen_presentation_controller_incoming_for(
            m_presentationState, frame.buffer_generation, &incoming);
        replacesPresentation = waywallen_presentation_controller_buffer_changes_with(
            m_presentationState, frame.buffer_generation);
        reuseCandidate = replacesPresentation && m_preparedEglContent.valid &&
                         m_preparedEglContent.buffer_generation == frame.buffer_generation;
    }
    auto resourcesOwner = renderSessionResources();
    if (! incoming.valid || ! resourcesOwner || m_activeBackend != BackendEGL ||
        ! m_eglImagesValid) {
        if (frame.acquireSyncFd >= 0) ::close(frame.acquireSyncFd);
        releaseEglFrame(frame.releaseSyncobjFd, false, frame.buffer_generation, frame.seq);
        return;
    }
    const int rc = waywallen_display_wait_egl_acquire(resourcesOwner->display, frame.acquireSyncFd);
    if (rc != WAYWALLEN_OK) {
        releaseEglFrame(frame.releaseSyncobjFd, false, frame.buffer_generation, frame.seq);
        QPointer<WaywallenDisplay> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, session = std::weak_ptr<RenderSessionResources>(resourcesOwner), rc]() {
                const auto owner = session.lock();
                if (guard && owner && guard->renderSessionResources() == owner)
                    guard->handleDisconnect(rc, "EGL acquire wait failed");
            },
            Qt::QueuedConnection);
        return;
    }
    // EGLImage → GL texture binding is render-thread work; safe here.
    if (! m_glTexturesCreated) ensureGlTextures();
    if (! m_glTexturesCreated || frame.slot < 0 || frame.slot >= m_glTextures.size() ||
        incoming.width <= 0 || incoming.height <= 0) {
        releaseEglFrame(frame.releaseSyncobjFd, false, frame.buffer_generation, frame.seq);
        return;
    }
    const EglBlitResult result = blitEglShadow(
        frame.slot, incoming.width, incoming.height, replacesPresentation, reuseCandidate);
    if (result == EglBlitResult::CurrentUpdated) {
        releaseEglFrame(frame.releaseSyncobjFd, true, frame.buffer_generation, frame.seq);
    } else if (result == EglBlitResult::CandidateReady) {
        {
            QMutexLocker lk(&m_pendingMutex);
            m_preparedEglContent = incoming;
        }
        releaseEglFrame(frame.releaseSyncobjFd, true, frame.buffer_generation, frame.seq);
    } else if (result == EglBlitResult::FailedAfterGpuWork) {
        releaseEglFrame(frame.releaseSyncobjFd, true, frame.buffer_generation, frame.seq);
    } else {
        releaseEglFrame(frame.releaseSyncobjFd, false, frame.buffer_generation, frame.seq);
    }
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

QSGNode* WaywallenDisplay::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto             resourcesOwner = renderSessionResources();
    auto*            resources      = resourcesOwner.get();
    const qulonglong frameSerial    = ++m_renderFrameSerial;
    int              frameSlot      = -1;
    int              framesInFlight = 1;
    if (window()) {
        const auto graphicsState = window()->graphicsStateInfo();
        frameSlot                = graphicsState.currentFrameSlot;
        framesInFlight           = qMax(1, graphicsState.framesInFlight);
    }
    // Run library-deferred pool destructions on the render thread,
    // where (a) Qt's GL context is current for glDeleteTextures (EGL
    // path) and (b) we can guarantee no in-flight vkQueueSubmit on
    // the blitter is still referencing the released VkImages (Vulkan
    // path). The library itself never calls vkDeviceWaitIdle from the
    // I/O thread anymore — that's what was racing with Qt's RHI on
    // radv and surfacing as VK_ERROR_DEVICE_LOST during rapid
    // wallpaper switches.
    if (resources && resources->display) {
#ifdef WW_HAVE_VULKAN
        // Vulkan: skip drain when the blitter's fence is in-flight —
        // its cmd buffer may still be reading the most recently
        // released pool's VkImage (only happens after a post-submit
        // timeout; in steady state fence is cleared between blits).
        // Next iteration's pre-submit wait will clear it and we
        // drain then.
        const bool blitterBusy =
            resources->vkPresenter && waywallen_vulkan_presenter_busy(resources->vkPresenter);
        if (! blitterBusy) {
            (void)waywallen_display_drain(resources->display);
        }
#else
        (void)waywallen_display_drain(resources->display);
#endif
    }

    auto* rootNode       = dynamic_cast<PresentationNode*>(oldNode);
    auto* transitionNode = dynamic_cast<TransitionNode*>(oldNode);
    if (! rootNode && ! transitionNode && oldNode) {
        delete oldNode;
        oldNode = nullptr;
    }

    ContentSnapshot outgoingContent;
    qulonglong      transitionSerial      = 0;
    bool            renderTransition      = false;
    qreal           transitionProgress    = 1.0;
    TransitionKind  activeTransitionKind  = NoTransition;
    quint32         activeTransitionAngle = 0;
    QPointF         activeTransitionOrigin;
    {
        QMutexLocker lock(&m_pendingMutex);
        renderTransition = m_activeTransitionSerial != 0 && m_outgoingContent.valid &&
                           (m_transitionActive || m_frameSubmissionUsesTransition);
        if (renderTransition) {
            outgoingContent        = m_outgoingContent;
            transitionSerial       = m_activeTransitionSerial;
            transitionProgress     = m_transitionProgress;
            activeTransitionKind   = m_activeTransitionKind;
            activeTransitionAngle  = m_activeTransitionAngle;
            activeTransitionOrigin = m_activeTransitionOrigin;
        }
    }

    if (transitionNode && (! renderTransition || transitionNode->serial() != transitionSerial)) {
        delete transitionNode;
        transitionNode = nullptr;
        oldNode        = nullptr;
        if (resources && resources->hasOutgoing()) {
            resources->retireOutgoing({ m_transitionLastFrameSlot, m_transitionLastFrameSerial });
            m_retirementPumpBudget = qMax(m_retirementPumpBudget, framesInFlight);
        }
        QMutexLocker lock(&m_pendingMutex);
        m_outgoingContent = ContentSnapshot {};
    }

    bool retirementPending = resources && resources->collectRetired(frameSlot, frameSerial);
    if (retirementPending && m_retirementPumpBudget > 0) {
        --m_retirementPumpBudget;
        QPointer<WaywallenDisplay> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, session = std::weak_ptr<RenderSessionResources>(resourcesOwner)]() {
                const auto owner = session.lock();
                if (guard && owner && guard->renderSessionResources() == owner) guard->update();
            },
            Qt::QueuedConnection);
    } else if (! retirementPending) {
        m_retirementPumpBudget = 0;
    }

    auto ensureRootNode = [&]() -> PresentationNode* {
        if (rootNode) return rootNode;
        if (! window()) return nullptr;
        rootNode = PresentationNode::create(window());
        oldNode  = rootNode;
        return rootNode;
    };
    auto scheduleSessionFailure =
        [this,
         session = std::weak_ptr<RenderSessionResources>(resourcesOwner)](const QString& reason) {
            QPointer<WaywallenDisplay> guard(this);
            QMetaObject::invokeMethod(
                this,
                [guard, session, reason]() {
                    const auto owner = session.lock();
                    if (guard && owner && guard->renderSessionResources() == owner)
                        guard->handleDisconnect(-EIO, reason.toUtf8().constData());
                },
                Qt::QueuedConnection);
        };

    ContentSnapshot eglCandidateContent;
    {
        QMutexLocker lk(&m_pendingMutex);
        if (m_preparedEglContent.valid) {
            ContentSnapshot latest;
            if (waywallen_presentation_controller_incoming_for(
                    m_presentationState, m_preparedEglContent.buffer_generation, &latest)) {
                eglCandidateContent = latest;
            }
        }
    }

#ifdef WW_HAVE_VULKAN
    PendingVkFrame  frame;
    ContentSnapshot incoming;
    bool            replacesPresentation = false;
    bool            reuseCandidate       = false;
    {
        QMutexLocker lk(&m_pendingMutex);
        frame       = m_pendingVk;
        m_pendingVk = PendingVkFrame {};
        if (frame.valid) {
            (void)waywallen_presentation_controller_incoming_for(
                m_presentationState, frame.buffer_generation, &incoming);
            replacesPresentation = waywallen_presentation_controller_buffer_changes_with(
                m_presentationState, frame.buffer_generation);
            reuseCandidate = replacesPresentation && m_preparedVkContent.valid &&
                             m_preparedVkContent.buffer_generation == frame.buffer_generation;
        }
    }

    const bool canBlitVk = frame.valid && incoming.valid && m_activeBackend == BackendVulkan &&
                           m_vkImagesValid && frame.slot >= 0 && frame.slot < m_vkImages.size() &&
                           incoming.width > 0 && incoming.height > 0;
    if (frame.valid && ! canBlitVk) {
        if (frame.releaseSyncobjFd >= 0) {
            signalFrameRelease(frame.releaseSyncobjFd,
                               frame.buffer_generation,
                               frame.seq,
                               "unusable Vulkan frame");
        }
    } else if (canBlitVk) {
        if (! resources || ! resources->vkPresenter) {
            if (! resources) {
                if (frame.releaseSyncobjFd >= 0) {
                    signalFrameRelease(frame.releaseSyncobjFd,
                                       frame.buffer_generation,
                                       frame.seq,
                                       "Vulkan frame without resources");
                }
                frame.valid = false;
            }
        }
        if (frame.valid && ! resources->vkPresenter) {
            const int rc = waywallen_vulkan_presenter_create(m_vkInstance,
                                                             m_vkPhys,
                                                             m_vkDevice,
                                                             m_vkQfi,
                                                             m_vkQueue,
                                                             m_vkGipa,
                                                             &resources->vkPresenter);
            if (rc != 0) {
                qCWarning(
                    lcWD, "vk blitter init failed (%d); Vulkan path disabled this session", rc);
                if (frame.releaseSyncobjFd >= 0) {
                    signalFrameRelease(frame.releaseSyncobjFd,
                                       frame.buffer_generation,
                                       frame.seq,
                                       "Vulkan blitter init failure");
                }
                frame.valid = false;
            }
        }

        if (frame.valid) {
            bool      candidateReady = false;
            bool      releaseArmed   = false;
            const int rc =
                waywallen_vulkan_presenter_prepare(resources->vkPresenter,
                                                   m_vkImages[frame.slot],
                                                   static_cast<uint32_t>(incoming.width),
                                                   static_cast<uint32_t>(incoming.height),
                                                   incoming.fourcc,
                                                   replacesPresentation,
                                                   reuseCandidate,
                                                   frame.acquireSem,
                                                   frame.releaseSyncobjFd,
                                                   &candidateReady,
                                                   &releaseArmed);
            if (releaseArmed) reportFrameArmed(frame.buffer_generation, frame.seq);
            if (rc == 0) {
                if (candidateReady) {
                    {
                        QMutexLocker lock(&m_pendingMutex);
                        m_preparedVkContent = incoming;
                    }
                    if (! reuseCandidate) {
                        waywallen_vulkan_presentable_descriptor_t currentDescriptor {};
                        waywallen_vulkan_presentable_descriptor_t candidateDescriptor {};
                        (void)waywallen_vulkan_presenter_current(resources->vkPresenter,
                                                                 &currentDescriptor);
                        (void)waywallen_vulkan_presenter_candidate(resources->vkPresenter,
                                                                   &candidateDescriptor);
                        qCInfo(lcWD,
                               "Vulkan shadow candidate: display=%llu %dx%d bytes=%llu "
                               "resident=%llu",
                               m_displayId,
                               incoming.width,
                               incoming.height,
                               qulonglong(candidateDescriptor.allocation_size),
                               qulonglong(currentDescriptor.allocation_size +
                                          candidateDescriptor.allocation_size));
                    }
                }
            } else if (! releaseArmed) {
                bool drainedRelease = false;
                (void)waywallen_vulkan_presenter_drain_pending_release(resources->vkPresenter,
                                                                       &drainedRelease);
                if (drainedRelease) reportFrameArmed(frame.buffer_generation, frame.seq);
                scheduleSessionFailure(QStringLiteral("Vulkan frame release could not be armed"));
            } else {
                waywallen_vulkan_presentable_descriptor_t candidateDescriptor {};
                if (waywallen_vulkan_presenter_candidate(resources->vkPresenter,
                                                         &candidateDescriptor) &&
                    waywallen_vulkan_presenter_discard_candidate(resources->vkPresenter) != 0) {
                    qCCritical(lcWD, "Vulkan shadow candidate remains in flight; ending session");
                    scheduleSessionFailure(QStringLiteral("Vulkan shadow candidate did not drain"));
                }
            }
        }
    }
#endif

#ifdef WW_HAVE_VULKAN
    ContentSnapshot vkCandidateContent;
    {
        QMutexLocker lock(&m_pendingMutex);
        if (m_preparedVkContent.valid) {
            ContentSnapshot latest;
            if (waywallen_presentation_controller_incoming_for(
                    m_presentationState, m_preparedVkContent.buffer_generation, &latest)) {
                vkCandidateContent = latest;
            }
        }
    }
#endif

    ContentSnapshot                        candidateContent;
    bool                                   candidateAvailable = false;
    waywallen_egl_presentable_descriptor_t eglCandidateDescriptor {};
    const bool                             hasEglCandidate =
        resources && resources->eglPresenter &&
        waywallen_egl_presenter_candidate(resources->eglPresenter, &eglCandidateDescriptor);
    if (m_activeBackend == BackendEGL && hasEglCandidate && eglCandidateDescriptor.has_content &&
        eglCandidateContent.valid) {
        candidateContent   = eglCandidateContent;
        candidateAvailable = true;
    }
#ifdef WW_HAVE_VULKAN
    if (resources && m_activeBackend == BackendVulkan && vkCandidateContent.valid &&
        resources->vkPresenter) {
        waywallen_vulkan_presentable_descriptor_t candidateDescriptor {};
        const bool                                candidateHasContent =
            waywallen_vulkan_presenter_candidate(resources->vkPresenter, &candidateDescriptor) &&
            candidateDescriptor.has_content;
        if (candidateHasContent) {
            candidateContent   = vkCandidateContent;
            candidateAvailable = true;
        }
    }
#endif

    if (! window()) candidateAvailable = false;

    if (hasEglCandidate && (! candidateAvailable || m_activeBackend != BackendEGL || ! window())) {
        resources->discardEglCandidate();
    }
#ifdef WW_HAVE_VULKAN
    if (resources && resources->vkPresenter &&
        (! candidateAvailable || m_activeBackend != BackendVulkan || ! window())) {
        waywallen_vulkan_presentable_descriptor_t candidateDescriptor {};
        if (waywallen_vulkan_presenter_candidate(resources->vkPresenter, &candidateDescriptor) &&
            waywallen_vulkan_presenter_discard_candidate(resources->vkPresenter) != 0) {
            scheduleSessionFailure(QStringLiteral("Vulkan shadow candidate cleanup failed"));
        }
    }
#endif

    if (candidateAvailable) {
        QMutexLocker    lock(&m_pendingMutex);
        ContentSnapshot prepared;
        bool            preparedTransition = false;
        if (m_candidatePrepareSerial != 0 &&
            ! waywallen_presentation_controller_prepared(
                m_presentationState, m_candidatePrepareSerial, &prepared, &preparedTransition)) {
            m_candidatePrepareSerial = 0;
            m_promotionSerial        = 0;
        }
        const bool transitionBusy =
            m_transitionActive || m_activeTransitionSerial != 0 ||
            m_frameSubmissionUsesTransition ||
            waywallen_presentation_controller_transition_active(m_presentationState);
        if (m_candidatePrepareSerial == 0 && ! transitionBusy) {
            std::uint64_t   serial = 0;
            ContentSnapshot displayed {};
            const bool      outgoingAvailable =
                waywallen_presentation_controller_displayed(m_presentationState, &displayed);
            const auto result = waywallen_presentation_controller_prepare(m_presentationState,
                                                                          &candidateContent,
                                                                          transitionConfigured(),
                                                                          outgoingAvailable,
                                                                          &serial);
            if (result == WAYWALLEN_PRESENTATION_PREPARE_DIRECT ||
                result == WAYWALLEN_PRESENTATION_PREPARE_TRANSITION ||
                result == WAYWALLEN_PRESENTATION_PREPARE_ALREADY_PREPARED) {
                m_candidatePrepareSerial = serial;
                m_promotionSerial        = serial;
                if (result == WAYWALLEN_PRESENTATION_PREPARE_ALREADY_PREPARED) {
                    ContentSnapshot ignored {};
                    bool            transition = false;
                    (void)waywallen_presentation_controller_prepared(
                        m_presentationState, serial, &ignored, &transition);
                    m_promotionUsesTransition = transition;
                } else {
                    m_promotionUsesTransition = result == WAYWALLEN_PRESENTATION_PREPARE_TRANSITION;
                }
            } else if (result == WAYWALLEN_PRESENTATION_PREPARE_REJECTED) {
                qCWarning(lcWD,
                          "candidate rejected for buffer=%llu token=%llu",
                          qulonglong(candidateContent.buffer_generation),
                          qulonglong(candidateContent.content_token));
            }
        }
    }

    qulonglong      promotionSerial     = 0;
    bool            promotionTransition = false;
    ContentSnapshot promotionOutgoing;
    if (candidateAvailable) {
        QMutexLocker lock(&m_pendingMutex);
        if (m_promotionSerial != 0 && m_promotionSerial == m_candidatePrepareSerial) {
            ContentSnapshot prepared;
            bool            preparedTransition = false;
            if (waywallen_presentation_controller_prepared(
                    m_presentationState, m_promotionSerial, &prepared, &preparedTransition) &&
                prepared.buffer_generation == candidateContent.buffer_generation) {
                promotionSerial     = m_promotionSerial;
                promotionTransition = m_promotionUsesTransition;
                (void)waywallen_presentation_controller_displayed(m_presentationState,
                                                                  &promotionOutgoing);
            }
        }
    }

    if (promotionSerial != 0) {
        delete oldNode;
        oldNode        = nullptr;
        rootNode       = nullptr;
        transitionNode = nullptr;

        bool committed = false;
        if (m_activeBackend == BackendEGL) {
            committed = resources->commitEglCandidateRetaining();
            if (committed) {
                waywallen_egl_presentable_descriptor_t currentDescriptor {};
                waywallen_egl_presentable_descriptor_t outgoingDescriptor {};
                (void)waywallen_egl_presenter_current(resources->eglPresenter, &currentDescriptor);
                (void)waywallen_egl_presentable_descriptor(resources->eglOutgoing,
                                                           &outgoingDescriptor);
                qCInfo(lcWD,
                       "EGL shadow submitted: display=%llu resident=%llu",
                       m_displayId,
                       qulonglong(currentDescriptor.allocation_size +
                                  outgoingDescriptor.allocation_size));
            }
        }
#ifdef WW_HAVE_VULKAN
        else if (m_activeBackend == BackendVulkan) {
            const int result =
                waywallen_vulkan_presenter_commit(resources->vkPresenter, &resources->vkOutgoing);
            committed = result == 0;
            if (! committed) {
                qCCritical(lcWD, "Vulkan shadow submit failed: %d", int(result));
            } else {
                waywallen_vulkan_presentable_descriptor_t currentDescriptor {};
                waywallen_vulkan_presentable_descriptor_t outgoingDescriptor {};
                (void)waywallen_vulkan_presenter_current(resources->vkPresenter,
                                                         &currentDescriptor);
                (void)waywallen_vulkan_presentable_descriptor(resources->vkOutgoing,
                                                              &outgoingDescriptor);
                qCInfo(lcWD,
                       "Vulkan shadow submitted: display=%llu resident=%llu",
                       m_displayId,
                       qulonglong(currentDescriptor.allocation_size +
                                  outgoingDescriptor.allocation_size));
            }
        }
#endif
        if (! committed) {
            {
                QMutexLocker lock(&m_pendingMutex);
                (void)waywallen_presentation_controller_discard(m_presentationState,
                                                                promotionSerial);
                m_candidatePrepareSerial  = 0;
                m_promotionSerial         = 0;
                m_promotionUsesTransition = false;
                m_preparedEglContent      = ContentSnapshot {};
#ifdef WW_HAVE_VULKAN
                m_preparedVkContent = ContentSnapshot {};
#endif
            }
            bool discarded = true;
            if (m_activeBackend == BackendEGL) {
                resources->discardEglCandidate();
            }
#ifdef WW_HAVE_VULKAN
            else if (m_activeBackend == BackendVulkan) {
                discarded =
                    waywallen_vulkan_presenter_discard_candidate(resources->vkPresenter) == 0;
            }
#endif
            qCWarning(lcWD, "shadow candidate promotion failed; keeping current content");
            if (! discarded) {
                scheduleSessionFailure(QStringLiteral("shadow candidate cleanup failed"));
            }
        } else {
            bool promoted = false;
            {
                QMutexLocker lock(&m_pendingMutex);
                promoted =
                    waywallen_presentation_controller_promote(m_presentationState, promotionSerial);
                if (promoted) {
                    m_candidatePrepareSerial  = 0;
                    m_promotionSerial         = 0;
                    m_promotionUsesTransition = false;
                    m_preparedEglContent      = ContentSnapshot {};
#ifdef WW_HAVE_VULKAN
                    m_preparedVkContent = ContentSnapshot {};
#endif
                    if (promotionTransition) {
                        m_outgoingContent            = promotionOutgoing;
                        m_activeTransitionSerial     = promotionSerial;
                        m_transitionActive           = false;
                        m_transitionProgress         = 0.0;
                        m_activeTransitionKind       = m_transitionKind;
                        m_activeTransitionDurationMs = m_transitionDurationMs;
                        m_activeTransitionAngle      = m_transitionAngle;
                        m_activeTransitionOrigin     = m_transitionOrigin;
                        waywallen_presentation_controller_set_transition_active(m_presentationState,
                                                                                true);
                    } else {
                        m_outgoingContent = ContentSnapshot {};
                    }
                }
            }
            if (! promoted) {
                scheduleSessionFailure(QStringLiteral("shadow candidate state was superseded"));
            } else {
                if (promotionTransition) {
                    renderTransition       = true;
                    outgoingContent        = promotionOutgoing;
                    transitionSerial       = promotionSerial;
                    transitionProgress     = 0.0;
                    activeTransitionKind   = m_activeTransitionKind;
                    activeTransitionAngle  = m_activeTransitionAngle;
                    activeTransitionOrigin = m_activeTransitionOrigin;
                }
                if (! promotionTransition && resources->hasOutgoing()) {
                    resources->retireOutgoing(
                        { m_presentedLastFrameSlot, m_presentedLastFrameSerial });
                    m_retirementPumpBudget = qMax(m_retirementPumpBudget, framesInFlight);
                }
                armPresentationSubmission(promotionSerial, promotionTransition);
            }
        }
    }

    ContentSnapshot presented {};
    {
        QMutexLocker lk(&m_pendingMutex);
        (void)waywallen_presentation_controller_displayed(m_presentationState, &presented);
    }

    waywallen_egl_presentable_descriptor_t currentEglDescriptor {};
    waywallen_egl_presentable_descriptor_t outgoingEglDescriptor {};
    const bool                             hasCurrentEgl =
        resources && resources->eglPresenter &&
        waywallen_egl_presenter_current(resources->eglPresenter, &currentEglDescriptor);
    const bool hasOutgoingEgl =
        resources && resources->eglOutgoing &&
        waywallen_egl_presentable_descriptor(resources->eglOutgoing, &outgoingEglDescriptor);

#ifdef WW_HAVE_VULKAN
    waywallen_vulkan_presentable_descriptor_t currentVkDescriptor {};
    waywallen_vulkan_presentable_descriptor_t outgoingVkDescriptor {};
    const bool                                hasCurrentVk =
        resources && resources->vkPresenter &&
        waywallen_vulkan_presenter_current(resources->vkPresenter, &currentVkDescriptor);
    const bool hasOutgoingVk =
        resources && resources->vkOutgoing &&
        waywallen_vulkan_presentable_descriptor(resources->vkOutgoing, &outgoingVkDescriptor);
#endif

    const bool hasTexture =
        // EGL gate: only expose the shadow once at least one frame has
        // been blitted into it, otherwise we'd sample uninitialized
        // GPU memory. After the first frame the shadow stays valid
        // across pool transitions, which is what gives the EGL path
        // the same "keep last frame on switch" continuity the Vulkan
        // path has.
        (presented.valid && m_activeBackend == BackendEGL && hasCurrentEgl &&
         currentEglDescriptor.has_content)
#ifdef WW_HAVE_VULKAN
        // Gate Vulkan sampling on a shadow populated by a completed copy.
        || (presented.valid && m_activeBackend == BackendVulkan && hasCurrentVk &&
            currentVkDescriptor.has_content)
#endif
        ;

    if (! hasTexture || ! window()) {
        delete oldNode;
        return nullptr;
    }

    auto createCurrentTexture = [&]() -> QSGTexture* {
        const QSize textureSize(presented.width, presented.height);
        if (m_activeBackend == BackendEGL) {
            return QNativeInterface::QSGOpenGLTexture::fromNative(
                currentEglDescriptor.texture,
                window(),
                textureSize,
                QQuickWindow::TextureHasAlphaChannel);
        }
#ifdef WW_HAVE_VULKAN
        if (m_activeBackend == BackendVulkan) {
            return QNativeInterface::QSGVulkanTexture::fromNative(
                reinterpret_cast<VkImage>(currentVkDescriptor.image),
                static_cast<VkImageLayout>(currentVkDescriptor.layout),
                window(),
                textureSize,
                QQuickWindow::TextureHasAlphaChannel);
        }
#endif
        return nullptr;
    };

    auto createOutgoingTexture = [&]() -> QSGTexture* {
        const QSize textureSize(outgoingContent.width, outgoingContent.height);
        if (! resources || ! resources->hasOutgoing()) return nullptr;
        if (m_activeBackend == BackendEGL) {
            if (! hasOutgoingEgl) return nullptr;
            return QNativeInterface::QSGOpenGLTexture::fromNative(
                outgoingEglDescriptor.texture,
                window(),
                textureSize,
                QQuickWindow::TextureHasAlphaChannel);
        }
#ifdef WW_HAVE_VULKAN
        if (m_activeBackend == BackendVulkan) {
            if (! hasOutgoingVk) return nullptr;
            return QNativeInterface::QSGVulkanTexture::fromNative(
                reinterpret_cast<VkImage>(outgoingVkDescriptor.image),
                static_cast<VkImageLayout>(outgoingVkDescriptor.layout),
                window(),
                textureSize,
                QQuickWindow::TextureHasAlphaChannel);
        }
#endif
        return nullptr;
    };

    const QRectF bounds = boundingRect();
    if (renderTransition) {
        if (! transitionNode || transitionNode->serial() != transitionSerial) {
            delete oldNode;
            oldNode        = nullptr;
            rootNode       = nullptr;
            transitionNode = TransitionNode::create(
                window(), transitionSerial, createOutgoingTexture(), createCurrentTexture());
            oldNode = transitionNode;
        }
        if (transitionNode) {
            transitionNode->updateScene(activeTransitionKind,
                                        transitionProgress,
                                        activeTransitionAngle,
                                        activeTransitionOrigin,
                                        bounds,
                                        outgoingContent,
                                        presented,
                                        m_displayWidth,
                                        m_displayHeight);
            m_presentedLastFrameSlot    = frameSlot;
            m_presentedLastFrameSerial  = frameSerial;
            m_transitionLastFrameSlot   = frameSlot;
            m_transitionLastFrameSerial = frameSerial;
            return transitionNode;
        }

        qCWarning(lcWD, "transition scene creation failed; presenting content directly");
        QMutexLocker lock(&m_pendingMutex);
        m_transitionActive = false;
        waywallen_presentation_controller_set_transition_active(m_presentationState, false);
        m_activeTransitionSerial        = 0;
        m_transitionProgress            = 1.0;
        m_frameSubmissionUsesTransition = false;
        m_outgoingContent               = ContentSnapshot {};
        lock.unlock();
        if (resources && resources->hasOutgoing()) {
            resources->retireOutgoing({ m_presentedLastFrameSlot, m_presentedLastFrameSerial });
            m_retirementPumpBudget = qMax(m_retirementPumpBudget, framesInFlight);
        }
    }

    rootNode = ensureRootNode();
    if (! rootNode) return nullptr;
    auto* clearNode = rootNode->clearNode;
    auto* xformNode = rootNode->transformNode;
    auto* node      = rootNode->imageNode;

    clearNode->setRect(bounds);
    clearNode->setColor(qtColor(presented.config.clear_color));

    const QSize texSize(presented.width, presented.height);

    if (m_activeBackend == BackendEGL) {
        const auto handle = static_cast<quintptr>(currentEglDescriptor.texture);
        if (! rootNode->wraps(PresentationNode::TextureBackend::OpenGL, handle, texSize)) {
            QSGTexture* wrapper = QNativeInterface::QSGOpenGLTexture::fromNative(
                currentEglDescriptor.texture,
                window(),
                texSize,
                QQuickWindow::TextureHasAlphaChannel);
            if (! wrapper) {
                delete rootNode;
                return nullptr;
            }
            rootNode->installTexture(
                wrapper, PresentationNode::TextureBackend::OpenGL, handle, texSize);
        }
    } else if (m_activeBackend == BackendVulkan) {
#ifdef WW_HAVE_VULKAN
        const auto image  = reinterpret_cast<VkImage>(currentVkDescriptor.image);
        const auto handle = reinterpret_cast<quintptr>(image);
        if (! rootNode->wraps(PresentationNode::TextureBackend::Vulkan, handle, texSize)) {
            QSGTexture* wrapper = QNativeInterface::QSGVulkanTexture::fromNative(
                image,
                static_cast<VkImageLayout>(currentVkDescriptor.layout),
                window(),
                texSize,
                QQuickWindow::TextureHasAlphaChannel);
            if (! wrapper) {
                delete rootNode;
                return nullptr;
            }
            rootNode->installTexture(
                wrapper, PresentationNode::TextureBackend::Vulkan, handle, texSize);
        }
#endif
    }

    PresentationNode::configureImage(
        node, xformNode, presented, bounds, m_displayWidth, m_displayHeight);

    m_presentedLastFrameSlot   = frameSlot;
    m_presentedLastFrameSerial = frameSerial;
    return rootNode;
}
