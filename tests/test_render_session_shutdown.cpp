#include "RenderSessionResources.hpp"

#include <QGuiApplication>
#include <QQuickWindow>
#include <memory>

Q_LOGGING_CATEGORY(lcWD, "waywallen.display.test")

static void check(bool condition, const char* message) {
    if (! condition) qFatal("%s", message);
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    auto            window    = std::make_unique<QQuickWindow>();
    auto            resources = std::make_shared<RenderSessionResources>();
    resources->window         = window.get();
    const waywallen_display_callbacks_t callbacks {};
    resources->display = waywallen_display_new(&callbacks);
    check(resources->display, "failed to create display");
    check(! resources->isShutdown(), "new session is already shut down");
    resources->invalidationConnection = QObject::connect(
        window.get(),
        &QQuickWindow::sceneGraphInvalidated,
        window.get(),
        [resources] {
            resources->shutdown();
        },
        Qt::DirectConnection);

    // Invalidation precedes item release while the window's QPointer is still valid.
    window->sceneGraphInvalidated();
    check(resources->isShutdown(), "invalidation did not finish session shutdown");
    check(resources->window == window.get(), "test requires a still-live window pointer");
    check(! resources->callbacksEnabled.load(), "callbacks remain enabled after shutdown");
    check(! resources->display, "display was not released");
    check(! resources->invalidationConnection, "invalidation connection was not cleared");

    window.reset();
    check(resources->window.isNull(), "window pointer was not cleared on destruction");
    // A job queued before invalidation can still run after the session has shut down.
    RenderSessionCleanupJob job(resources);
    job.run();
    resources->shutdown();
    check(resources->isShutdown(), "shutdown state was lost");
    return 0;
}
