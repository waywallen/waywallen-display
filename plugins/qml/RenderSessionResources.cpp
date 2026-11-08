#include "RenderSessionResources.hpp"

#include <QOpenGLExtraFunctions>
#include <QQuickWindow>
#include <utility>

namespace
{
QOpenGLExtraFunctions* glFunctions(void* userData) {
    return static_cast<QOpenGLExtraFunctions*>(userData);
}

void getInteger(void* userData, uint32_t name, int32_t* value) {
    glFunctions(userData)->glGetIntegerv(static_cast<GLenum>(name), value);
}

bool isEnabled(void* userData, uint32_t capability) {
    return glFunctions(userData)->glIsEnabled(static_cast<GLenum>(capability)) == GL_TRUE;
}

void enable(void* userData, uint32_t capability) {
    glFunctions(userData)->glEnable(static_cast<GLenum>(capability));
}

void disable(void* userData, uint32_t capability) {
    glFunctions(userData)->glDisable(static_cast<GLenum>(capability));
}

void genTextures(void* userData, int32_t count, uint32_t* textures) {
    glFunctions(userData)->glGenTextures(count, textures);
}

void deleteTextures(void* userData, int32_t count, const uint32_t* textures) {
    glFunctions(userData)->glDeleteTextures(count, textures);
}

void bindTexture(void* userData, uint32_t target, uint32_t texture) {
    glFunctions(userData)->glBindTexture(static_cast<GLenum>(target), texture);
}

void textureParameter(void* userData, uint32_t target, uint32_t name, int32_t value) {
    glFunctions(userData)->glTexParameteri(
        static_cast<GLenum>(target), static_cast<GLenum>(name), value);
}

void textureImage2d(void* userData, uint32_t target, int32_t level, int32_t internalFormat,
                    int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type,
                    const void* data) {
    glFunctions(userData)->glTexImage2D(static_cast<GLenum>(target),
                                        level,
                                        internalFormat,
                                        width,
                                        height,
                                        border,
                                        static_cast<GLenum>(format),
                                        static_cast<GLenum>(type),
                                        data);
}

void genFramebuffers(void* userData, int32_t count, uint32_t* framebuffers) {
    glFunctions(userData)->glGenFramebuffers(count, framebuffers);
}

void deleteFramebuffers(void* userData, int32_t count, const uint32_t* framebuffers) {
    glFunctions(userData)->glDeleteFramebuffers(count, framebuffers);
}

void bindFramebuffer(void* userData, uint32_t target, uint32_t framebuffer) {
    glFunctions(userData)->glBindFramebuffer(static_cast<GLenum>(target), framebuffer);
}

void framebufferTexture2d(void* userData, uint32_t target, uint32_t attachment,
                          uint32_t textureTarget, uint32_t texture, int32_t level) {
    glFunctions(userData)->glFramebufferTexture2D(static_cast<GLenum>(target),
                                                  static_cast<GLenum>(attachment),
                                                  static_cast<GLenum>(textureTarget),
                                                  texture,
                                                  level);
}

uint32_t checkFramebuffer(void* userData, uint32_t target) {
    return glFunctions(userData)->glCheckFramebufferStatus(static_cast<GLenum>(target));
}

void blitFramebuffer(void* userData, int32_t sourceX0, int32_t sourceY0, int32_t sourceX1,
                     int32_t sourceY1, int32_t destX0, int32_t destY0, int32_t destX1,
                     int32_t destY1, uint32_t mask, uint32_t filter) {
    glFunctions(userData)->glBlitFramebuffer(sourceX0,
                                             sourceY0,
                                             sourceX1,
                                             sourceY1,
                                             destX0,
                                             destY0,
                                             destX1,
                                             destY1,
                                             static_cast<GLbitfield>(mask),
                                             static_cast<GLenum>(filter));
}

uint32_t getError(void* userData) { return glFunctions(userData)->glGetError(); }

const waywallen_gl_functions_t kGlFunctions {
    getInteger,
    isEnabled,
    enable,
    disable,
    genTextures,
    deleteTextures,
    bindTexture,
    textureParameter,
    textureImage2d,
    genFramebuffers,
    deleteFramebuffers,
    bindFramebuffer,
    framebufferTexture2d,
    checkFramebuffer,
    blitFramebuffer,
    getError,
};
} // namespace

RenderSessionResources::~RenderSessionResources() {
    if (display || eglPresenter
#ifdef WW_HAVE_VULKAN
        || vkPresenter
#endif
    ) {
        qCCritical(lcWD,
                   "render-session cleanup job was discarded; GPU resources are retained until "
                   "process exit");
    }
}

void RenderSessionResources::shutdown() {
    callbacksEnabled.store(false, std::memory_order_release);
    QObject::disconnect(invalidationConnection);
    invalidationConnection = {};
#ifdef WW_HAVE_VULKAN
    if (vkPresenter) {
        (void)waywallen_vulkan_presenter_drain_pending_release(vkPresenter, nullptr);
        waywallen_vulkan_presenter_release(vkPresenter, vkOutgoing);
        vkOutgoing = nullptr;
        vkRetired.drain([this](waywallen_vulkan_presentable_t*& presentable) {
            waywallen_vulkan_presenter_release(vkPresenter, presentable);
        });
        waywallen_vulkan_presenter_destroy(vkPresenter);
        vkPresenter = nullptr;
    }
#endif
    if (display) {
        waywallen_display_shutdown(display);
        display = nullptr;
    }
    if (eglPresenter) {
        waywallen_egl_presenter_release(eglPresenter, eglOutgoing);
        eglOutgoing = nullptr;
        eglRetired.drain([this](waywallen_egl_presentable_t*& presentable) {
            waywallen_egl_presenter_release(eglPresenter, presentable);
        });
        waywallen_egl_presenter_destroy(eglPresenter);
        eglPresenter = nullptr;
    }
    eglDisplay = nullptr;
}

bool RenderSessionResources::ensureEglPresenter(QOpenGLExtraFunctions* functions) {
    if (eglPresenter) return true;
    return functions &&
           waywallen_egl_presenter_create(&kGlFunctions, functions, &eglPresenter) == 0;
}

bool RenderSessionResources::discardEglCandidate() {
    if (! eglPresenter) return true;
    waywallen_egl_presenter_discard_candidate(eglPresenter);
    return true;
}

bool RenderSessionResources::commitEglCandidateRetaining() {
    return eglPresenter && waywallen_egl_presenter_commit(eglPresenter, &eglOutgoing) == 0;
}

bool RenderSessionResources::hasOutgoing() const {
    bool present = eglOutgoing != nullptr;
#ifdef WW_HAVE_VULKAN
    present = present || vkOutgoing != nullptr;
#endif
    return present;
}

void RenderSessionResources::retireOutgoing(const FrameSlotRetirement& retirement) {
    if (eglOutgoing) eglRetired.retire(std::exchange(eglOutgoing, nullptr), retirement);
#ifdef WW_HAVE_VULKAN
    if (vkOutgoing) vkRetired.retire(std::exchange(vkOutgoing, nullptr), retirement);
#endif
}

bool RenderSessionResources::collectRetired(int frameSlot, std::uint64_t frameSerial) {
    if (eglPresenter) {
        eglRetired.collect(
            frameSlot, frameSerial, [this](waywallen_egl_presentable_t*& presentable) {
                waywallen_egl_presenter_release(eglPresenter, presentable);
            });
    }
#ifdef WW_HAVE_VULKAN
    if (vkPresenter) {
        vkRetired.collect(
            frameSlot, frameSerial, [this](waywallen_vulkan_presentable_t*& presentable) {
                waywallen_vulkan_presenter_release(vkPresenter, presentable);
            });
    }
#endif
    bool pending = ! eglRetired.empty();
#ifdef WW_HAVE_VULKAN
    pending = pending || ! vkRetired.empty();
#endif
    return pending;
}

RenderSessionCleanupJob::RenderSessionCleanupJob(std::shared_ptr<RenderSessionResources> resources)
    : m_resources(std::move(resources)) {
    setAutoDelete(true);
}

void RenderSessionCleanupJob::run() {
    m_resources->shutdown();
    m_resources.reset();
}
