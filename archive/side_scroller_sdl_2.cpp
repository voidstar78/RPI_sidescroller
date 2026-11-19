#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Color {
    Uint8 r = 0;
    Uint8 g = 0;
    Uint8 b = 0;
    Uint8 a = 255;
};

struct SegmentConfig {
    std::string text;
    std::string font = "DejaVu Sans Mono";
    int size = 72;
    Color color{0, 0, 0, 255};
    int gapAfter = 24;
};

struct Config {
    int windowWidth = 1280;
    int windowHeight = 240;
    Color background{255, 255, 255, 255};
    double pixelsPerSecond = 160.0;
    int startPadding = 120;
    int endPadding = 120;
    int repeatPauseMs = 700;
    double globalScale = 1.0;
    std::vector<SegmentConfig> segments;
};

static void printUsage(const char* exe) {
    std::cout
        << "Usage: " << exe << " [config.xml]

"
        << "If omitted, the program loads scroller_config.xml from the current folder.

"
        << "Keys:
"
        << "  Left/Right arrows  Decrease/increase speed
"
        << "  Up/Down arrows     Increase/decrease text scale
"
        << "  Space              Pause/resume scrolling
"
        << "  R                  Reload XML config
"
        << "  Esc or close       Quit
";
}

static std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" 	
");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" 	
");
    return s.substr(first, last - first + 1);
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::string xmlUnescape(std::string s) {
    struct Pair { const char* from; const char* to; };
    const Pair pairs[] = {
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&amp;", "&"}
    };

    for (const auto& p : pairs) {
        std::string::size_type pos = 0;
        while ((pos = s.find(p.from, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(p.from), p.to);
            pos += std::strlen(p.to);
        }
    }
    return s;
}

static std::map<std::string, std::string> parseAttributes(const std::string& tagInside) {
    std::map<std::string, std::string> attrs;
    std::size_t i = 0;

    while (i < tagInside.size() && !std::isspace(static_cast<unsigned char>(tagInside[i]))) {
        ++i;
    }

    while (i < tagInside.size()) {
        while (i < tagInside.size() && std::isspace(static_cast<unsigned char>(tagInside[i]))) ++i;
        if (i >= tagInside.size()) break;
        if (tagInside[i] == '/' || tagInside[i] == '>') break;

        std::size_t keyStart = i;
        while (i < tagInside.size() && tagInside[i] != '=' && !std::isspace(static_cast<unsigned char>(tagInside[i]))) ++i;
        std::string key = tagInside.substr(keyStart, i - keyStart);
        while (i < tagInside.size() && std::isspace(static_cast<unsigned char>(tagInside[i]))) ++i;
        if (i >= tagInside.size() || tagInside[i] != '=') break;
        ++i;
        while (i < tagInside.size() && std::isspace(static_cast<unsigned char>(tagInside[i]))) ++i;
        if (i >= tagInside.size()) break;

        char quote = tagInside[i];
        if (quote != '"' && quote != '\'') break;
        ++i;
        std::size_t valueStart = i;
        while (i < tagInside.size() && tagInside[i] != quote) ++i;
        std::string value = tagInside.substr(valueStart, i - valueStart);
        if (i < tagInside.size()) ++i;

        attrs[toLower(trim(key))] = xmlUnescape(value);
    }

    return attrs;
}

static std::optional<std::string> getAttribute(const std::map<std::string, std::string>& attrs, const std::string& key) {
    auto it = attrs.find(toLower(key));
    if (it == attrs.end()) return std::nullopt;
    return it->second;
}

static int parseInt(const std::optional<std::string>& value, int fallback) {
    if (!value || trim(*value).empty()) return fallback;
    return std::stoi(trim(*value));
}

static double parseDouble(const std::optional<std::string>& value, double fallback) {
    if (!value || trim(*value).empty()) return fallback;
    return std::stod(trim(*value));
}

static Uint8 hexByte(const std::string& s, std::size_t pos) {
    return static_cast<Uint8>(std::stoi(s.substr(pos, 2), nullptr, 16));
}

static Color parseColor(const std::optional<std::string>& value, Color fallback) {
    if (!value) return fallback;
    std::string s = trim(*value);
    if (s.empty()) return fallback;

    if (s[0] == '#') s.erase(0, 1);
    if (s.size() == 6) {
        return Color{hexByte(s, 0), hexByte(s, 2), hexByte(s, 4), 255};
    }
    if (s.size() == 8) {
        return Color{hexByte(s, 0), hexByte(s, 2), hexByte(s, 4), hexByte(s, 6)};
    }
    return fallback;
}

static std::string readFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Could not open file: " + path + " (" + std::strerror(errno) + ")");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string extractTagContent(const std::string& xml, const std::string& tagName) {
    const std::string openTag = "<" + tagName;
    const std::string closeTag = "</" + tagName + ">";
    std::size_t openPos = xml.find(openTag);
    if (openPos == std::string::npos) return "";
    std::size_t openEnd = xml.find('>', openPos);
    if (openEnd == std::string::npos) return "";
    std::size_t closePos = xml.find(closeTag, openEnd + 1);
    if (closePos == std::string::npos) return "";
    return xml.substr(openEnd + 1, closePos - (openEnd + 1));
}

static std::vector<std::pair<std::map<std::string, std::string>, std::string>> extractSegmentTags(const std::string& xml) {
    std::vector<std::pair<std::map<std::string, std::string>, std::string>> out;
    std::size_t pos = 0;

    while (true) {
        std::size_t open = xml.find("<segment", pos);
        if (open == std::string::npos) break;
        std::size_t tagEnd = xml.find('>', open);
        if (tagEnd == std::string::npos) break;

        std::string tagInside = xml.substr(open + 1, tagEnd - open - 1);
        bool selfClosing = !tagInside.empty() && tagInside.back() == '/';
        auto attrs = parseAttributes(tagInside);

        if (selfClosing) {
            out.push_back({attrs, ""});
            pos = tagEnd + 1;
            continue;
        }

        std::size_t close = xml.find("</segment>", tagEnd + 1);
        if (close == std::string::npos) break;
        std::string content = xml.substr(tagEnd + 1, close - (tagEnd + 1));
        out.push_back({attrs, xmlUnescape(trim(content))});
        pos = close + std::string("</segment>").size();
    }

    return out;
}

static Config loadConfig(const std::string& path) {
    Config cfg;
    std::string xml = readFileText(path);

    std::string windowContent = extractTagContent(xml, "window");
    if (!windowContent.empty()) {
        std::size_t open = xml.find("<window");
        if (open != std::string::npos) {
            std::size_t end = xml.find('>', open);
            if (end != std::string::npos) {
                auto attrs = parseAttributes(xml.substr(open + 1, end - open - 1));
                cfg.windowWidth = std::max(320, parseInt(getAttribute(attrs, "width"), cfg.windowWidth));
                cfg.windowHeight = std::max(100, parseInt(getAttribute(attrs, "height"), cfg.windowHeight));
                cfg.background = parseColor(getAttribute(attrs, "background"), cfg.background);
            }
        }
    }

    std::size_t scrollerOpen = xml.find("<scroller");
    if (scrollerOpen != std::string::npos) {
        std::size_t scrollerEnd = xml.find('>', scrollerOpen);
        if (scrollerEnd != std::string::npos) {
            auto attrs = parseAttributes(xml.substr(scrollerOpen + 1, scrollerEnd - scrollerOpen - 1));
            cfg.pixelsPerSecond = std::max(5.0, parseDouble(getAttribute(attrs, "speed"), cfg.pixelsPerSecond));
            cfg.startPadding = std::max(0, parseInt(getAttribute(attrs, "startPadding"), cfg.startPadding));
            cfg.endPadding = std::max(0, parseInt(getAttribute(attrs, "endPadding"), cfg.endPadding));
            cfg.repeatPauseMs = std::max(0, parseInt(getAttribute(attrs, "repeatPauseMs"), cfg.repeatPauseMs));
            cfg.globalScale = std::max(0.2, parseDouble(getAttribute(attrs, "scale"), cfg.globalScale));
        }
    }

    auto segments = extractSegmentTags(xml);
    for (const auto& item : segments) {
        SegmentConfig seg;
        const auto& attrs = item.first;
        seg.text = item.second;
        if (auto v = getAttribute(attrs, "text")) seg.text = *v;
        if (auto v = getAttribute(attrs, "font")) seg.font = *v;
        seg.size = std::max(8, parseInt(getAttribute(attrs, "size"), seg.size));
        seg.color = parseColor(getAttribute(attrs, "color"), seg.color);
        seg.gapAfter = std::max(0, parseInt(getAttribute(attrs, "gapAfter"), seg.gapAfter));
        if (!trim(seg.text).empty()) {
            seg.text = xmlUnescape(seg.text);
            cfg.segments.push_back(seg);
        }
    }

    if (cfg.segments.empty()) {
        throw std::runtime_error("XML config contains no <segment> entries with text.");
    }

    return cfg;
}

static std::vector<fs::path> candidateFontDirs() {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    dirs.emplace_back("C:/Windows/Fonts");
#else
    dirs.emplace_back("./fonts");
    dirs.emplace_back(".");
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    if (const char* home = std::getenv("HOME")) {
        dirs.emplace_back(fs::path(home) / ".fonts");
        dirs.emplace_back(fs::path(home) / ".local/share/fonts");
    }
#endif
    return dirs;
}

static bool isFontExtension(const fs::path& p) {
    const std::string ext = toLower(p.extension().string());
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

static std::optional<fs::path> findFontByNameOrPath(const std::string& nameOrPath) {
    fs::path direct(nameOrPath);
    if (fs::exists(direct) && fs::is_regular_file(direct)) {
        return fs::absolute(direct);
    }

    const std::string target = toLower(nameOrPath);
    std::string targetTight;
    for (char c : target) {
        if (c != ' ' && c != '-' && c != '_') targetTight.push_back(c);
    }

    for (const auto& dir : candidateFontDirs()) {
        if (!fs::exists(dir)) continue;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file()) continue;
            const fs::path p = it->path();
            if (!isFontExtension(p)) continue;

            std::string stem = toLower(p.stem().string());
            std::string tight;
            for (char c : stem) {
                if (c != ' ' && c != '-' && c != '_') tight.push_back(c);
            }

            if (stem == target || stem.find(target) != std::string::npos ||
                tight == targetTight || tight.find(targetTight) != std::string::npos) {
                return p;
            }
        }
    }

    return std::nullopt;
}

class FontHandle {
public:
    FontHandle() = default;
    explicit FontHandle(TTF_Font* font) : font_(font) {}
    ~FontHandle() { reset(); }

    FontHandle(const FontHandle&) = delete;
    FontHandle& operator=(const FontHandle&) = delete;

    FontHandle(FontHandle&& other) noexcept : font_(other.font_) { other.font_ = nullptr; }
    FontHandle& operator=(FontHandle&& other) noexcept {
        if (this != &other) {
            reset();
            font_ = other.font_;
            other.font_ = nullptr;
        }
        return *this;
    }

    void reset(TTF_Font* font = nullptr) {
        if (font_) TTF_CloseFont(font_);
        font_ = font;
    }

    TTF_Font* get() const { return font_; }

private:
    TTF_Font* font_ = nullptr;
};

class TextureHandle {
public:
    TextureHandle() = default;
    explicit TextureHandle(SDL_Texture* texture) : texture_(texture) {}
    ~TextureHandle() { reset(); }

    TextureHandle(const TextureHandle&) = delete;
    TextureHandle& operator=(const TextureHandle&) = delete;

    TextureHandle(TextureHandle&& other) noexcept : texture_(other.texture_) { other.texture_ = nullptr; }
    TextureHandle& operator=(TextureHandle&& other) noexcept {
        if (this != &other) {
            reset();
            texture_ = other.texture_;
            other.texture_ = nullptr;
        }
        return *this;
    }

    void reset(SDL_Texture* texture = nullptr) {
        if (texture_) SDL_DestroyTexture(texture_);
        texture_ = texture;
    }

    SDL_Texture* get() const { return texture_; }

private:
    SDL_Texture* texture_ = nullptr;
};

struct RenderedSegment {
    std::string text;
    std::string fontLabel;
    int requestedSize = 72;
    Color color{0, 0, 0, 255};
    int gapAfter = 24;
    int width = 0;
    int height = 0;
    TextureHandle texture;
};

static int computeScaledFontSize(int windowHeight, int baseSize, double globalScale) {
    int target = static_cast<int>(std::round(windowHeight * 0.60 * globalScale));
    int boundedBase = std::max(8, baseSize);
    return std::max(8, std::min(std::max(boundedBase, target), boundedBase * 6));
}

static bool buildRenderedSegments(
    SDL_Renderer* renderer,
    const Config& cfg,
    int windowHeight,
    std::vector<RenderedSegment>& outSegments,
    std::string& errorMessage) {

    outSegments.clear();

    for (const auto& seg : cfg.segments) {
        auto fontPathOpt = findFontByNameOrPath(seg.font);
        if (!fontPathOpt) {
            errorMessage = "Could not find font: " + seg.font;
            return false;
        }

        int actualSize = computeScaledFontSize(windowHeight, seg.size, cfg.globalScale);
        FontHandle font(TTF_OpenFont(fontPathOpt->string().c_str(), actualSize));
        if (!font.get()) {
            errorMessage = std::string("TTF_OpenFont failed for '") + seg.font + "': " + TTF_GetError();
            return false;
        }

        SDL_Color sdlColor{seg.color.r, seg.color.g, seg.color.b, seg.color.a};
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font.get(), seg.text.c_str(), sdlColor);
        if (!surf) {
            errorMessage = std::string("TTF_RenderUTF8_Blended failed: ") + TTF_GetError();
            return false;
        }

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surf);
        if (!texture) {
            errorMessage = std::string("SDL_CreateTextureFromSurface failed: ") + SDL_GetError();
            SDL_FreeSurface(surf);
            return false;
        }

        RenderedSegment rs;
        rs.text = seg.text;
        rs.fontLabel = seg.font;
        rs.requestedSize = actualSize;
        rs.color = seg.color;
        rs.gapAfter = seg.gapAfter;
        rs.width = surf->w;
        rs.height = surf->h;
        rs.texture.reset(texture);
        SDL_FreeSurface(surf);
        outSegments.push_back(std::move(rs));
    }

    errorMessage.clear();
    return true;
}

static int totalRenderedWidth(const Config& cfg, const std::vector<RenderedSegment>& segments) {
    int total = cfg.startPadding + cfg.endPadding;
    for (const auto& seg : segments) {
        total += seg.width + seg.gapAfter;
    }
    if (!segments.empty()) total -= segments.back().gapAfter;
    return total;
}

static void renderFrame(
    SDL_Renderer* renderer,
    const Config& cfg,
    const std::vector<RenderedSegment>& segments,
    int windowW,
    int windowH,
    double offsetX) {

    SDL_SetRenderDrawColor(renderer, cfg.background.r, cfg.background.g, cfg.background.b, cfg.background.a);
    SDL_RenderClear(renderer);

    int x = static_cast<int>(std::round(offsetX)) + cfg.startPadding;
    for (const auto& seg : segments) {
        int y = (windowH - seg.height) / 2;
        SDL_Rect dst{x, y, seg.width, seg.height};
        SDL_RenderCopy(renderer, seg.texture.get(), nullptr, &dst);
        x += seg.width + seg.gapAfter;
    }

    SDL_RenderPresent(renderer);
}

int main(int argc, char** argv) {
    std::string configPath = "scroller_config.xml";
    if (argc >= 2) {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        configPath = arg;
    }

    Config cfg;
    try {
        cfg = loadConfig(configPath);
    } catch (const std::exception& ex) {
        std::cerr << "Config error: " << ex.what() << "
";
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "
";
        return 1;
    }

    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init failed: " << TTF_GetError() << "
";
        SDL_Quit();
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Side Scroller",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        cfg.windowWidth,
        cfg.windowHeight,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "
";
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "
";
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    int windowW = cfg.windowWidth;
    int windowH = cfg.windowHeight;
    bool paused = false;
    bool waitingToRepeat = false;
    Uint32 repeatPauseStartTicks = 0;
    double speed = cfg.pixelsPerSecond;

    std::vector<RenderedSegment> renderedSegments;
    std::string errorMessage;
    if (!buildRenderedSegments(renderer, cfg, windowH, renderedSegments, errorMessage)) {
        std::cerr << errorMessage << "
";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    double offsetX = static_cast<double>(windowW);
    Uint64 lastCounter = SDL_GetPerformanceCounter();
    bool running = true;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        windowW = e.window.data1;
                        windowH = e.window.data2;
                        if (!buildRenderedSegments(renderer, cfg, windowH, renderedSegments, errorMessage)) {
                            std::cerr << errorMessage << "
";
                            running = false;
                        }
                    }
                    break;

                case SDL_KEYDOWN:
                    switch (e.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            running = false;
                            break;
                        case SDLK_SPACE:
                            paused = !paused;
                            break;
                        case SDLK_LEFT:
                            speed = std::max(5.0, speed - 20.0);
                            break;
                        case SDLK_RIGHT:
                            speed = std::min(3000.0, speed + 20.0);
                            break;
                        case SDLK_UP:
                            cfg.globalScale = std::min(4.0, cfg.globalScale + 0.10);
                            if (!buildRenderedSegments(renderer, cfg, windowH, renderedSegments, errorMessage)) {
                                std::cerr << errorMessage << "
";
                                running = false;
                            }
                            break;
                        case SDLK_DOWN:
                            cfg.globalScale = std::max(0.20, cfg.globalScale - 0.10);
                            if (!buildRenderedSegments(renderer, cfg, windowH, renderedSegments, errorMessage)) {
                                std::cerr << errorMessage << "
";
                                running = false;
                            }
                            break;
                        case SDLK_r:
                        case SDLK_R:
                            try {
                                cfg = loadConfig(configPath);
                                speed = cfg.pixelsPerSecond;
                                if (!buildRenderedSegments(renderer, cfg, windowH, renderedSegments, errorMessage)) {
                                    std::cerr << errorMessage << "
";
                                    running = false;
                                }
                                offsetX = static_cast<double>(windowW);
                                waitingToRepeat = false;
                            } catch (const std::exception& ex) {
                                std::cerr << "Reload error: " << ex.what() << "
";
                            }
                            break;
                    }
                    break;
            }
        }

        Uint64 nowCounter = SDL_GetPerformanceCounter();
        const double dt = static_cast<double>(nowCounter - lastCounter) /
                          static_cast<double>(SDL_GetPerformanceFrequency());
        lastCounter = nowCounter;

        const int contentWidth = totalRenderedWidth(cfg, renderedSegments);

        if (!paused) {
            if (waitingToRepeat) {
                Uint32 elapsed = SDL_GetTicks() - repeatPauseStartTicks;
                if (elapsed >= static_cast<Uint32>(cfg.repeatPauseMs)) {
                    offsetX = static_cast<double>(windowW);
                    waitingToRepeat = false;
                }
            } else {
                offsetX -= speed * dt;
                if (offsetX + contentWidth < 0.0) {
                    waitingToRepeat = true;
                    repeatPauseStartTicks = SDL_GetTicks();
                }
            }
        }

        renderFrame(renderer, cfg, renderedSegments, windowW, windowH, offsetX);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}

/*
Example XML file: scroller_config.xml

<?xml version="1.0" encoding="UTF-8"?>
<scrollerApp>
  <window width="1280" height="240" background="#FFFFFF" />

  <scroller speed="160"
            startPadding="180"
            endPadding="180"
            repeatPauseMs="900"
            scale="1.0" />

  <segment text="WELCOME TO THE EVENT" font="DejaVu Sans Mono" size="72" color="#000000" gapAfter="50" />
  <segment text=" • " font="DejaVu Sans Mono" size="72" color="#666666" gapAfter="20" />
  <segment text="SPECIAL GUEST ARRIVING SOON" font="DejaVu Serif" size="76" color="#C00000" gapAfter="50" />
  <segment font="DejaVu Sans Mono" size="68" color="#0044AA" gapAfter="40">PLEASE TAKE YOUR SEATS</segment>
</scrollerApp>

Build on Raspberry Pi OS:
  sudo apt update
  sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev
  g++ -std=c++17 side_scroller_sdl2.cpp -o side_scroller $(pkg-config --cflags --libs sdl2 SDL2_ttf)

Run:
  ./side_scroller
or:
  ./side_scroller my_config.xml
*/
