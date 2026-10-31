#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string toLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

static std::string trim(std::string s) {
	auto notSpace = [](unsigned char c) { return !std::isspace(c); };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
	s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
	return s;
}

// Splits "silent, nine circles" into {"silent", "nine circles"} (lowercased)
static std::vector<std::string> splitKeywords(std::string const& raw) {
	std::vector<std::string> out;
	std::stringstream ss(raw);
	std::string part;
	while (std::getline(ss, part, ',')) {
		part = toLower(trim(part));
		if (!part.empty()) out.push_back(part);
	}
	return out;
}

static void openConsole() {
#ifdef GEODE_IS_WINDOWS
	static bool opened = false;
	if (opened) return;
	opened = true;
	// If Geode's own console is already open this simply fails, which is fine.
	AllocConsole();
	FILE* f = nullptr;
	freopen_s(&f, "CONOUT$", "w", stdout);
	freopen_s(&f, "CONOUT$", "w", stderr);
	SetConsoleTitleA("Level ID Scanner");
	std::ios::sync_with_stdio(true);
	std::cout.clear();
#endif
}

static void consoleLine(std::string const& line) {
	std::cout << line << std::endl;
	log::info("{}", line);
}

// matches are saved to: found_levels.txt inside the mod's save folder
static std::filesystem::path foundFilePath() {
	return Mod::get()->getSaveDir() / "found_levels.txt";
}

static void appendToFile(std::string const& line) {
	auto path = foundFilePath();
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec); // make the folder if it's missing
	std::ofstream out(path, std::ios::app);
	if (out) {
		out << line << "\n";
	}
	else {
		consoleLine(fmt::format("WARNING: couldn't write to {}", path.string()));
	}
}

// ---------------------------------------------------------------------------
// scanner: one counter, walking backwards from the starting ID
// ---------------------------------------------------------------------------

class Scanner : public CCObject, public LevelManagerDelegate {
public:
	static Scanner* get() {
		static Scanner* instance = new Scanner();
		return instance;
	}

	bool isRunning() const { return m_running; }

	void start(int startId, std::vector<std::string> keywords) {
		this->stop(false);

		m_keywords = std::move(keywords);
		m_next = startId;
		m_checked = 0;
		m_found = 0;
		m_levelsSeen = 0;
		m_unlistedSeen = 0;
		m_failed = 0;
		m_clock = 0.0;
		m_tokens = 1.0;
		m_inflight.clear();
		m_running = true;

		std::string kw;
		for (size_t i = 0; i < m_keywords.size(); i++) {
			if (i) kw += ", ";
			kw += m_keywords[i];
		}
		consoleLine(fmt::format("Scanning backwards from ID {} for keyword(s): {}", startId, kw));
		consoleLine(fmt::format("Saving matches to: {}", foundFilePath().string()));
		appendToFile(fmt::format("--- Scan from ID {} for keyword(s): {} ---", startId, kw));

		CCDirector::get()->getScheduler()->scheduleSelector(
			schedule_selector(Scanner::tick), this, 0.05f, kCCRepeatForever, 0.f, false
		);
	}

	void stop(bool announce) {
		if (!m_running) return;
		m_running = false;
		CCDirector::get()->getScheduler()->unscheduleSelector(schedule_selector(Scanner::tick), this);
		auto glm = GameLevelManager::get();
		if (glm->m_levelManagerDelegate == this) glm->m_levelManagerDelegate = nullptr;
		m_inflight.clear();

		if (announce) {
			consoleLine(fmt::format(
				"Scan stopped at ID {}. Checked {} IDs, found {} match(es). "
				"(levels returned: {}, flagged unlisted: {}, empty/failed replies: {})",
				m_next, m_checked, m_found, m_levelsSeen, m_unlistedSeen, m_failed
			));
		}
	}

	// LevelManagerDelegate ---------------------------------------------------
	void loadLevelsFinished(CCArray* levels, char const* key) override {
		this->handleResult(levels, key, false);
	}
	void loadLevelsFinished(CCArray* levels, char const* key, int) override {
		this->handleResult(levels, key, false);
	}
	void loadLevelsFailed(char const* key) override {
		this->handleResult(nullptr, key, true);
	}
	void loadLevelsFailed(char const* key, int) override {
		this->handleResult(nullptr, key, true);
	}
	void setupPageInfo(gd::string, char const*) override {}

private:
	struct Pending {
		int id;
		double sentAt;
	};

	bool m_running = false;
	int m_next = 0;        // next ID to request
	int m_checked = 0;
	int m_found = 0;
	int m_levelsSeen = 0;
	int m_unlistedSeen = 0;
	int m_failed = 0;
	int m_nextReport = 250;
	double m_clock = 0.0;
	double m_tokens = 1.0;
	std::map<std::string, Pending> m_inflight;
	std::vector<std::string> m_keywords;

	void sendSingle(int id) {
		// Searching for a plain number returns that exact level (same as the in-game search bar)
		auto search = GJSearchObject::create(SearchType::Search, std::to_string(id));
		std::string key = search->getKey();
		m_inflight[key] = Pending { id, m_clock };

		auto glm = GameLevelManager::get();
		glm->m_levelManagerDelegate = this; // another menu may have stolen it
		glm->getOnlineLevels(search);
	}

	void tick(float dt) {
		if (!m_running) return;
		m_clock += dt;

		// give up on lookups that never got an answer (10 seconds)
		for (auto it = m_inflight.begin(); it != m_inflight.end();) {
			if (m_clock - it->second.sentAt > 10.0) {
				Pending p = it->second;
				it = m_inflight.erase(it);
				this->deliver(p, nullptr, true);
				if (!m_running) return;
			}
			else ++it;
		}

		// finished?
		if (m_next < 1 && m_inflight.empty()) {
			consoleLine(fmt::format(
				"Reached ID 1. Done! Checked {} IDs, found {} match(es).", m_checked, m_found
			));
			this->stop(false);
			return;
		}
		if (m_next < 1) return;

		// a single speed limit: a token bucket refilled at "requests per second"
		double rps = Mod::get()->getSettingValue<double>("requests_per_second");
		m_tokens = std::min(m_tokens + dt * rps, 2.0);
		if (m_tokens < 1.0 || m_inflight.size() >= 3) return;

		m_tokens -= 1.0;
		int id = m_next--;
		this->sendSingle(id);
	}

	void handleResult(CCArray* levels, char const* key, bool failed) {
		if (!m_running || !key) return;
		auto it = m_inflight.find(key);
		if (it == m_inflight.end()) return;
		Pending p = it->second;
		m_inflight.erase(it);
		this->deliver(p, levels, failed);
	}

	void deliver(Pending const& p, CCArray* levels, bool failed) {
		bool onlyUnlisted = Mod::get()->getSettingValue<bool>("only_unlisted");
		bool gotLevels = false;
		if (!failed && levels) {
			for (auto level : CCArrayExt<GJGameLevel*>(levels)) {
				gotLevels = true;
				m_levelsSeen++;
				if (level->m_unlisted) m_unlistedSeen++;
				if (onlyUnlisted && !level->m_unlisted) continue; // publicly listed: ignore
				std::string name = level->m_levelName;
				std::string lowered = toLower(name);
				for (auto const& kw : m_keywords) {
					if (lowered.find(kw) != std::string::npos) {
						m_found++;
						consoleLine(fmt::format(
							"LEVEL FOUND MY BRUDA!!!! : {}, {}", level->m_levelID.value(), name
						));
						appendToFile(fmt::format("{}, {}", level->m_levelID.value(), name));
						break;
					}
				}
			}
		}

		if (!gotLevels) m_failed++;
		m_checked++;

		if (m_checked >= m_nextReport) {
			m_nextReport += 250;
			consoleLine(fmt::format(
				"... now at ID {} | checked {} | levels returned {} | flagged unlisted {} | empty/failed replies {}",
				m_next, m_checked, m_levelsSeen, m_unlistedSeen, m_failed
			));
		}
	}
};

// ---------------------------------------------------------------------------
// popup: "Starting ID:" and "Keywords:"
// ---------------------------------------------------------------------------

class ScannerPopup : public Popup {
protected:
	TextInput* m_idInput = nullptr;
	TextInput* m_keywordInput = nullptr;

	static inline std::string s_lastId;
	static inline std::string s_lastKeywords;

	bool init() {
		if (!Popup::init(340.f, 240.f)) return false;
		this->setTitle("Level ID Scanner");

		auto idLabel = CCLabelBMFont::create("Starting ID:", "bigFont.fnt");
		idLabel->setScale(0.5f);
		m_mainLayer->addChildAtPosition(idLabel, Anchor::Center, {0.f, 62.f});

		m_idInput = TextInput::create(250.f, "e.g. 151067814", "chatFont.fnt");
		m_idInput->setCommonFilter(CommonFilter::Uint);
		m_idInput->setString(s_lastId);
		m_mainLayer->addChildAtPosition(m_idInput, Anchor::Center, {0.f, 35.f});

		auto kwLabel = CCLabelBMFont::create("Keywords:", "bigFont.fnt");
		kwLabel->setScale(0.5f);
		m_mainLayer->addChildAtPosition(kwLabel, Anchor::Center, {0.f, 5.f});

		m_keywordInput = TextInput::create(250.f, "e.g. silent  (separate several with commas)", "chatFont.fnt");
		m_keywordInput->setMaxCharCount(100);
		m_keywordInput->setString(s_lastKeywords);
		m_mainLayer->addChildAtPosition(m_keywordInput, Anchor::Center, {0.f, -22.f});

		auto startBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Start"), this, menu_selector(ScannerPopup::onStart)
		);
		auto stopBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Stop"), this, menu_selector(ScannerPopup::onStop)
		);
		m_buttonMenu->addChildAtPosition(startBtn, Anchor::Center, {-50.f, -75.f});
		m_buttonMenu->addChildAtPosition(stopBtn, Anchor::Center, {50.f, -75.f});

		return true;
	}

	void onStart(CCObject*) {
		std::string idText = trim(m_idInput->getString());
		auto startId = numFromString<int>(idText);
		if (idText.empty() || !startId || startId.unwrap() < 1) {
			FLAlertLayer::create("Oops", "Enter a valid <cy>starting ID</c>.", "OK")->show();
			return;
		}

		auto keywords = splitKeywords(m_keywordInput->getString());
		if (keywords.empty()) {
			FLAlertLayer::create("Oops", "Enter at least one <cy>keyword</c>.", "OK")->show();
			return;
		}

		s_lastId = idText;
		s_lastKeywords = m_keywordInput->getString();

		openConsole();
		Scanner::get()->start(startId.unwrap(), keywords);
		this->onClose(nullptr);
	}

	void onStop(CCObject*) {
		openConsole();
		Scanner::get()->stop(true);
		this->onClose(nullptr);
	}

public:
	static ScannerPopup* create() {
		auto ret = new ScannerPopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

// ---------------------------------------------------------------------------
// main menu button
// ---------------------------------------------------------------------------

class $modify(ScannerMenuLayer, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;

		auto spr = ButtonSprite::create("ID Scan", "bigFont.fnt", "GJ_button_04.png", 0.7f);
		spr->setScale(0.7f);
		auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ScannerMenuLayer::onOpenScanner));
		btn->setID("id-scanner-button"_spr);

		if (auto menu = this->getChildByID("bottom-menu")) {
			menu->addChild(btn);
			menu->updateLayout();
		}
		return true;
	}

	void onOpenScanner(CCObject*) {
		ScannerPopup::create()->show();
	}
};
