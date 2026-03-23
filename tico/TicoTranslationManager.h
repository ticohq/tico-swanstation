#pragma once

#include <string>
#include <unordered_map>

class TicoTranslationManager {
public:
    static TicoTranslationManager& Instance();

    bool Init();
    std::string GetString(const std::string& key) const;

private:
    TicoTranslationManager() = default;
    
    std::string m_currentLanguage;
    std::unordered_map<std::string, std::string> m_translations;
};

// Global helper
std::string tr(const std::string& key);
