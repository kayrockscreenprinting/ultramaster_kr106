#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <mutex>
#include <vector>

struct KR106Preset
{
    juce::String name;
    std::vector<float> values;
};

class KR106PresetManager
{
public:
    std::vector<KR106Preset> mPresets;
    mutable std::mutex mMutex;

    static juce::File getAppDataDir()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("KR106");
    }

    static juce::File getDefaultCSVPath()
    {
        return getAppDataDir().getChildFile("patchbank.csv");
    }

    static juce::File getSettingsFile()
    {
        return getAppDataDir().getChildFile("settings.json");
    }

    // Returns the active CSV path (persisted or default)
    juce::File getActiveCSVPath() const
    {
        return mActiveCSVPath.existsAsFile() ? mActiveCSVPath : getDefaultCSVPath();
    }

    void setActiveCSVPath(const juce::File& path)
    {
        mActiveCSVPath = path;
        saveSetting("presetFile", path.getFullPathName());
    }

    // --- Global settings (JSON) ---

    static juce::var loadSettings()
    {
        auto file = getSettingsFile();
        if (!file.existsAsFile()) return juce::var();
        auto parsed = juce::JSON::parse(file.loadFileAsString());
        return parsed;
    }

    static void saveSetting(const juce::String& key, const juce::var& value)
    {
        auto settings = loadSettings();
        if (!settings.isObject())
            settings = juce::var(new juce::DynamicObject());
        settings.getDynamicObject()->setProperty(key, value);
        getAppDataDir().createDirectory();
        getSettingsFile().replaceWithText(juce::JSON::toString(settings));
    }

    static juce::var getSetting(const juce::String& key, const juce::var& defaultValue = {})
    {
        auto settings = loadSettings();
        if (settings.isObject() && settings.getDynamicObject()->hasProperty(key))
            return settings.getDynamicObject()->getProperty(key);
        return defaultValue;
    }

    // Bank prefix from position: A11..A88, B11..B88
    static juce::String getPrefix(int idx)
    {
        char group = static_cast<char>('A' + idx / 64);
        int bank = (idx % 64) / 8 + 1;
        int patch = idx % 8 + 1;
        return juce::String::charToString(group) + juce::String(bank) + juce::String(patch);
    }

    juce::String getDisplayName(int idx) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (idx < 0 || idx >= (int)mPresets.size()) return {};
        return getPrefix(idx) + " " + mPresets[idx].name;
    }

    // Strip "A11 " etc. from factory preset names
    static juce::String stripFactoryPrefix(const char* name)
    {
        juce::String s(name);
        if (s.length() >= 4 &&
            s[0] >= 'A' && s[0] <= 'B' &&
            s[1] >= '1' && s[1] <= '8' &&
            s[2] >= '1' && s[2] <= '8' &&
            s[3] == ' ')
            return s.substring(4);
        return s;
    }

    void initializeFromDisk(juce::RangedAudioParameter** params, int numParams,
                            const bool* exclude)
    {
        // Ensure factory CSV exists
        auto defaultCSV = getDefaultCSVPath();
        if (!defaultCSV.existsAsFile())
        {
            defaultCSV.getParentDirectory().createDirectory();
            writeFactory(defaultCSV, params, numParams, exclude);
        }

        // Load persisted path from settings, fall back to default
        auto savedPath = getSetting("presetFile").toString();
        if (savedPath.isNotEmpty())
        {
            juce::File saved(savedPath);
            if (saved.existsAsFile())
                mActiveCSVPath = saved;
        }

        // Migrate old settings.txt if present
        auto oldSettings = getAppDataDir().getChildFile("settings.txt");
        if (oldSettings.existsAsFile())
        {
            if (mActiveCSVPath == juce::File())
            {
                juce::File old(oldSettings.loadFileAsString().trim());
                if (old.existsAsFile())
                {
                    mActiveCSVPath = old;
                    saveSetting("presetFile", old.getFullPathName());
                }
            }
            oldSettings.deleteFile();
        }

        auto csvFile = getActiveCSVPath();
        loadFromFile(csvFile, params, numParams);
    }

    bool loadFromFile(const juce::File& file, juce::RangedAudioParameter** params, int numParams)
    {
        auto text = file.loadFileAsString();
        if (text.isEmpty()) return false;

        auto lines = juce::StringArray::fromLines(text);

        // Find header row (skip comments and blanks)
        int headerIdx = 0;
        while (headerIdx < lines.size())
        {
            auto trimmed = lines[headerIdx].trim();
            if (!trimmed.isEmpty() && !trimmed.startsWith("#"))
                break;
            headerIdx++;
        }
        if (headerIdx >= lines.size()) return false;

        auto headers = juce::StringArray::fromTokens(lines[headerIdx], ",", "");
        if (headers.size() < 2) return false;

        // Map column names to param indices
        std::vector<int> colToParam(headers.size(), -1);
        for (int c = 1; c < headers.size(); c++)
        {
            juce::String colName = headers[c].trim();
            for (int p = 0; p < numParams; p++)
            {
                if (params[p]->getName(100) == colName)
                {
                    colToParam[c] = p;
                    break;
                }
            }
        }

        // Parse data rows
        std::vector<KR106Preset> loaded;
        loaded.reserve(128);
        for (int i = headerIdx + 1; i < lines.size() && (int)loaded.size() < 128; i++)
        {
            auto line = lines[i].trim();
            if (line.isEmpty() || line.startsWith("#")) continue;

            auto fields = juce::StringArray::fromTokens(line, ",", "");
            if (fields.size() < 2) continue;

            KR106Preset preset;
            preset.name = fields[0].trim();
            preset.values.assign(numParams, 0.f);

            for (int c = 1; c < fields.size() && c < headers.size(); c++)
            {
                int paramIdx = colToParam[c];
                if (paramIdx < 0 || paramIdx >= numParams) continue;

                juce::String val = fields[c].trim();
                if (val.startsWith("[") && val.endsWith("]"))
                {
                    int midiVal = val.substring(1, val.length() - 1).getIntValue();
                    if (dynamic_cast<juce::AudioParameterFloat*>(params[paramIdx]))
                        preset.values[paramIdx] = midiVal / 127.f;
                    else
                        preset.values[paramIdx] = static_cast<float>(midiVal);
                }
                else
                {
                    preset.values[paramIdx] = val.getFloatValue();
                }
            }

            loaded.push_back(std::move(preset));
        }

        // Pad to 128 if needed
        while ((int)loaded.size() < 128)
        {
            KR106Preset init;
            init.name = "Init";
            init.values.assign(numParams, 0.f);
            loaded.push_back(std::move(init));
        }

        std::lock_guard<std::mutex> lock(mMutex);
        mPresets = std::move(loaded);
        return true;
    }

    bool saveToFile(const juce::File& file, juce::RangedAudioParameter** params,
                    int numParams, const bool* exclude) const
    {
        juce::String csv;
        csv += "name";
        for (int i = 0; i < numParams; i++)
            if (!exclude || !exclude[i])
                csv += "," + params[i]->getName(100);
        csv += "\n";

        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& p : mPresets)
            csv += formatPresetLine(p, params, numParams, exclude) + "\n";

        file.getParentDirectory().createDirectory();
        return file.replaceWithText(csv);
    }

    void captureCurrentParams(int index, const juce::String& name,
                              juce::RangedAudioParameter** params, int numParams)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index < 0 || index >= (int)mPresets.size()) return;
        mPresets[index].name = name.removeCharacters(",");
        mPresets[index].values.resize(numParams);
        for (int i = 0; i < numParams; i++)
            mPresets[index].values[i] = params[i]->convertFrom0to1(params[i]->getValue());
    }

    void renamePreset(int index, const juce::String& name)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index < 0 || index >= (int)mPresets.size()) return;
        mPresets[index].name = name.removeCharacters(",");
    }

    void clearPreset(int index, int numParams)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index < 0 || index >= (int)mPresets.size()) return;
        mPresets[index].name = "Init";
        mPresets[index].values.assign(numParams, 0.f);
    }

    KR106Preset getPreset(int index) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index >= 0 && index < (int)mPresets.size())
            return mPresets[index];
        return {};
    }

    void setPreset(int index, const KR106Preset& preset)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index >= 0 && index < (int)mPresets.size())
            mPresets[index] = preset;
    }

    // Replace a single data line in the CSV, preserving all other lines
    bool saveOnePreset(int index, const juce::File& file,
                       juce::RangedAudioParameter** params, int numParams,
                       const bool* exclude) const
    {
        auto text = file.loadFileAsString();
        if (text.isEmpty()) return saveToFile(file, params, numParams, exclude);

        auto lines = juce::StringArray::fromLines(text);

        // Find header row (skip comments and blanks)
        int headerIdx = 0;
        while (headerIdx < lines.size())
        {
            auto trimmed = lines[headerIdx].trim();
            if (!trimmed.isEmpty() && !trimmed.startsWith("#"))
                break;
            headerIdx++;
        }
        if (headerIdx >= lines.size()) return saveToFile(file, params, numParams, exclude);

        // Find the data line for this preset index (skip comments/blanks)
        int dataCount = 0;
        int targetLine = -1;
        for (int i = headerIdx + 1; i < lines.size(); i++)
        {
            auto trimmed = lines[i].trim();
            if (trimmed.isEmpty() || trimmed.startsWith("#")) continue;
            if (dataCount == index) { targetLine = i; break; }
            dataCount++;
        }
        if (targetLine < 0) return saveToFile(file, params, numParams, exclude);

        // Build the replacement line
        std::lock_guard<std::mutex> lock(mMutex);
        if (index < 0 || index >= (int)mPresets.size()) return false;
        lines.set(targetLine, formatPresetLine(mPresets[index], params, numParams, exclude));

        // Remove trailing empty lines that fromLines may have added
        while (lines.size() > 0 && lines[lines.size() - 1].isEmpty())
            lines.remove(lines.size() - 1);

        file.getParentDirectory().createDirectory();
        return file.replaceWithText(lines.joinIntoString("\n") + "\n");
    }

private:
    juce::String formatPresetLine(const KR106Preset& p,
                                  juce::RangedAudioParameter** params,
                                  int numParams, const bool* exclude) const
    {
        juce::String line = p.name.removeCharacters(",");
        for (int i = 0; i < numParams; i++)
        {
            if (exclude && exclude[i]) continue;
            line += ",";
            float val = (i < (int)p.values.size()) ? p.values[i] : 0.f;

            if (dynamic_cast<juce::AudioParameterInt*>(params[i]) ||
                dynamic_cast<juce::AudioParameterBool*>(params[i]))
            {
                line += "[" + juce::String(static_cast<int>(val)) + "]";
            }
            else
            {
                auto* fp = dynamic_cast<juce::AudioParameterFloat*>(params[i]);
                if (fp)
                {
                    auto range = fp->getNormalisableRange();
                    if (range.start == 0.f && range.end == 1.f && val >= 0.f && val <= 1.01f)
                    {
                        int midi = juce::roundToInt(val * 127.f);
                        float quantized = midi / 127.f;
                        if (std::abs(val - quantized) < 0.002f)
                            line += "[" + juce::String(midi) + "]";
                        else
                            line += juce::String(val, 6);
                    }
                    else
                        line += juce::String(val, 6);
                }
                else
                {
                    line += juce::String(val, 6);
                }
            }
        }
        return line;
    }

    juce::File mActiveCSVPath;

    // writeFactory is now identical to saveToFile — kept as alias for clarity
    bool writeFactory(const juce::File& file, juce::RangedAudioParameter** params,
                      int numParams, const bool* exclude) const
    {
        return saveToFile(file, params, numParams, exclude);
    }
};
