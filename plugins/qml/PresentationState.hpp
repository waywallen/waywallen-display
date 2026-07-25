#pragma once

#include <QColor>
#include <QRectF>
#include <cstdint>

class PresentationState {
public:
    struct Config {
        bool          valid { false };
        std::uint64_t bufferGeneration { 0 };
        std::uint64_t configGeneration { 0 };
        QRectF        sourceRect;
        QRectF        destRect;
        QColor        clearColor { Qt::black };
        std::uint32_t transform { 0 };
    };

    struct Content {
        bool          valid { false };
        std::uint64_t bufferGeneration { 0 };
        int           width { 0 };
        int           height { 0 };
        std::uint32_t fourcc { 0 };
        Config        config;
    };

    enum class ConfigResult
    {
        Rejected,
        Staged,
        PresentedUpdated,
    };

    enum class CommitResult
    {
        Rejected,
        SameSourceUpdated,
        SourceChanged,
    };

    void beginIncoming(std::uint64_t generation, int width, int height, std::uint32_t fourcc,
                       bool valid) {
        m_incoming.valid            = valid;
        m_incoming.bufferGeneration = generation;
        m_incoming.width            = width;
        m_incoming.height           = height;
        m_incoming.fourcc           = fourcc;
        m_incoming.config           = Config {};
    }

    void retireIncoming(std::uint64_t generation) {
        if (m_incoming.bufferGeneration == generation) m_incoming = Content {};
    }

    ConfigResult applyConfig(const Config& config) {
        if (! m_incoming.valid || m_incoming.bufferGeneration != config.bufferGeneration) {
            return ConfigResult::Rejected;
        }
        m_incoming.config = config;
        if (m_presented.valid && m_presented.bufferGeneration == config.bufferGeneration) {
            m_presented.config = config;
            return ConfigResult::PresentedUpdated;
        }
        return ConfigResult::Staged;
    }

    bool incomingFor(std::uint64_t generation, Content& out) const {
        if (! m_incoming.valid || ! m_incoming.config.valid ||
            m_incoming.bufferGeneration != generation) {
            return false;
        }
        out = m_incoming;
        return true;
    }

    CommitResult commit(const Content& content) {
        if (! content.valid || ! content.config.valid ||
            content.bufferGeneration != content.config.bufferGeneration || ! m_incoming.valid ||
            ! m_incoming.config.valid || content.bufferGeneration != m_incoming.bufferGeneration ||
            content.config.configGeneration != m_incoming.config.configGeneration) {
            return CommitResult::Rejected;
        }
        const bool sourceChanged =
            ! m_presented.valid || m_presented.bufferGeneration != content.bufferGeneration;
        m_presented = content;
        return sourceChanged ? CommitResult::SourceChanged : CommitResult::SameSourceUpdated;
    }

    bool sourceChangesWith(std::uint64_t generation) const {
        return ! m_presented.valid || m_presented.bufferGeneration != generation;
    }

    Content presented() const { return m_presented; }

    void reset() {
        m_incoming  = Content {};
        m_presented = Content {};
    }

private:
    Content m_incoming;
    Content m_presented;
};
