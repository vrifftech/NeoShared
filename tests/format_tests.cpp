#include "AppModel.hpp"
#include "GFFFile.hpp"
#include "GffJson.hpp"
#include "GffXml.hpp"
#include "SimpleJson.hpp"
#include "SimpleXml.hpp"
#include "neotlk/TlkFile.hpp"
#include "neotlk/TlkLookup.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void requireThrows(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}


void testGffResourceExtensions() {
    struct Expected {
        const char* extension;
        const char* fileType;
    };

    const Expected jadeResources[] = {
        {"qst", "QST "},
        {"qst2", "QST "},
        {"pla", "PLA "},
        {"cre", "CRE "},
        {"trg", "TRG "},
        {"dlg", "DLG "},
        {"fsm", "FSM "},
        {"gff", "GFF "},
        {"are", "ARE "},
        {"gui", "GUI "},
        {"sto", "STO "},
        {"cwa", "CWA "},
        {"cwd", "CWD "},
        {"sav", "SAV "},
    };

    const Expected otherClassicResources[] = {
        {"cam", "UTW "},
        {"uta", "UTA "},
        {"utx", "UTX "},
        {"gic", "GIC "},
    };

    const auto& jadeExtensions = neogff::jadeEmpireGffResourceExtensions();
    for (const Expected& expected : jadeResources) {
        require(neogff::isKnownGffResourceExtension(expected.extension),
                std::string("missing GFF extension: ") + expected.extension);
        std::string upperExtension = expected.extension;
        std::transform(upperExtension.begin(), upperExtension.end(), upperExtension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        require(neogff::isKnownGffResourceExtension("." + upperExtension),
                std::string("missing uppercase GFF extension: ") + upperExtension);
        const std::filesystem::path samplePath =
            std::string("resource.") + expected.extension;
        require(neogff::defaultGffTypeForExtension(samplePath) == expected.fileType,
                std::string("incorrect GFF type for extension: ") + expected.extension);
        require(std::find(jadeExtensions.begin(), jadeExtensions.end(), expected.extension) !=
                    jadeExtensions.end(),
                std::string("missing Jade GFF extension: ") + expected.extension);
    }

    require(neogff::preferredGffExtensionForType("QST") == "qst",
            "QST did not prefer the .qst extension");
    require(neogff::preferredGffExtensionForType("PLA ") == "pla",
            "PLA did not prefer the .pla extension");
    require(neogff::preferredGffExtensionForType("CRE") == "cre",
            "CRE did not prefer the .cre extension");
    require(neogff::preferredGffExtensionForType("TRG") == "trg",
            "TRG did not prefer the .trg extension");
    require(neogff::preferredGffExtensionForType("DLG") == "dlg",
            "DLG did not prefer the .dlg extension");
    require(neogff::preferredGffExtensionForType("FSM") == "fsm",
            "FSM did not prefer the .fsm extension");

    for (const Expected& expected : otherClassicResources) {
        require(neogff::isKnownGffResourceExtension(expected.extension),
                std::string("missing GFF extension: ") + expected.extension);
        std::string upperExtension = expected.extension;
        std::transform(upperExtension.begin(), upperExtension.end(), upperExtension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        require(neogff::isKnownGffResourceExtension("." + upperExtension),
                std::string("missing uppercase GFF extension: ") + upperExtension);
        const std::filesystem::path samplePath =
            std::string("resource.") + expected.extension;
        require(neogff::defaultGffTypeForExtension(samplePath) == expected.fileType,
                std::string("incorrect GFF type for extension: ") + expected.extension);
    }

    for (const char* nonGff : {"art", "lyt", "vis", "bip", "amp", "ndb",
                               "wfx", "rim", "trx"}) {
        require(!neogff::isKnownGffResourceExtension(nonGff),
                std::string("non-GFF Jade resource was misclassified: ") + nonGff);
    }
}


void testClassicGffV33RoundTrip() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "neoshared-gff-v33-roundtrip.utc";
    const std::filesystem::path importedPath =
        std::filesystem::temp_directory_path() / "neoshared-gff-v33-imported.utc";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(importedPath, ec);

    try {
        neogff::GffFile source;
        source.NewFile("UTC ", path);
        source.version("V3.3");
        neogff::GffStruct* root = source.root();
        require(root != nullptr, "synthetic GFF V3.3 root is missing");
        root->AddField(std::make_unique<neogff::GffExoStringField>("Name", "Witcher V3.3"));
        root->AddField(std::make_unique<neogff::GffIntField>("Value", 33));
        source.SaveFile(path);

        neogff::GffFile loaded;
        loaded.LoadFile(path);
        require(loaded.version() == "V3.3", "GFF V3.3 version was not preserved on load");
        require(loaded.filetype() == "UTC ", "GFF V3.3 type was not preserved on load");
        const neogff::GffField* valueField = loaded.root()->GetFieldByLabel("Value");
        require(valueField != nullptr && valueField->GetString() == "33",
                "GFF V3.3 field data did not round-trip");

        const std::string xml = neogff::ToGffXml(loaded);
        require(xml.find("version=\"V3.3\"") != std::string::npos,
                "GFF XML export did not preserve V3.3");
        const std::string json = neogff::gffXmlToJson(xml);
        require(json.find("\"version\": \"V3.3\"") != std::string::npos,
                "GFF JSON export did not preserve V3.3");

        neogff::GffFile imported;
        neogff::LoadGffXml(imported, neogff::gffJsonToXml(json));
        require(imported.version() == "V3.3",
                "GFF JSON/XML import did not restore V3.3");
        imported.SaveFile(importedPath);

        neogff::GffFile reopened;
        reopened.LoadFile(importedPath);
        require(reopened.version() == "V3.3",
                "imported GFF V3.3 file did not retain its version after save");
    } catch (...) {
        std::filesystem::remove(path, ec);
        std::filesystem::remove(importedPath, ec);
        throw;
    }

    std::filesystem::remove(path, ec);
    std::filesystem::remove(importedPath, ec);
}

void testDuplicateGffLabels() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "neoshared-duplicate-labels.sav";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    try {
        neogff::GffModel model;
        model.newFile("SAV ");
        neogff::GffStruct* root = model.gff().root();
        require(root != nullptr, "synthetic SAV root is missing");
        root->AddField(std::make_unique<neogff::GffIntField>("Repeated", 10));
        root->AddField(std::make_unique<neogff::GffIntField>("Repeated", 20));
        require(root->GetFieldByLabel("Repeated", 0) != nullptr,
                "first duplicate field occurrence was not retained");
        require(root->GetFieldByLabel("Repeated", 1) != nullptr,
                "second duplicate field occurrence was not retained");
        model.save(path);

        neogff::GffModel reopened;
        reopened.load(path);
        const auto rows = reopened.rows();
        const auto first = std::find_if(rows.begin(), rows.end(), [](const neogff::GffFieldRow& row) {
            return row.path == "Repeated[#1]";
        });
        const auto second = std::find_if(rows.begin(), rows.end(), [](const neogff::GffFieldRow& row) {
            return row.path == "Repeated[#2]";
        });
        require(first != rows.end() && first->value == "10",
                "first duplicate field occurrence was not exposed through AppModel");
        require(second != rows.end() && second->value == "20",
                "second duplicate field occurrence was not exposed through AppModel");

        reopened.setValue("Repeated[#2]", "25");
        reopened.save(path);
        neogff::GffModel edited;
        edited.load(path);
        const auto editedRows = edited.rows();
        const auto editedSecond = std::find_if(editedRows.begin(), editedRows.end(), [](const neogff::GffFieldRow& row) {
            return row.path == "Repeated[#2]";
        });
        require(editedSecond != editedRows.end() && editedSecond->value == "25",
                "occurrence-aware duplicate edit did not round-trip");
    } catch (...) {
        std::filesystem::remove(path, ec);
        throw;
    }

    std::filesystem::remove(path, ec);
}

void testDocumentDepthLimits() {
    std::string validJson(256, '[');
    validJson += '0';
    validJson.append(256, ']');
    (void)neojson::parse(validJson);

    std::string excessiveJson(257, '[');
    excessiveJson.append(257, ']');
    requireThrows([&] { (void)neojson::parse(excessiveJson); },
                  "excessively nested JSON was accepted");

    std::string validXml;
    for (int i = 0; i < 256; ++i) validXml += "<n>";
    validXml += "value";
    for (int i = 0; i < 256; ++i) validXml += "</n>";
    (void)neoxml::parse(validXml);

    std::string excessiveXml;
    for (int i = 0; i < 257; ++i) excessiveXml += "<n>";
    excessiveXml += "value";
    for (int i = 0; i < 257; ++i) excessiveXml += "</n>";
    requireThrows([&] { (void)neoxml::parse(excessiveXml); },
                  "excessively nested XML was accepted");
}

void testMissingRootStructId() {
    neogff::GffFile xmlGff;
    neogff::LoadGffXml(xmlGff, "<gff3 type=\"UTC\"><struct/></gff3>");
    require(xmlGff.root() != nullptr && xmlGff.root()->typeid_ == UINT32_MAX,
            "XML import changed a missing root struct ID to zero");

    const std::string json =
        "{\"fileType\":\"UTC\",\"root\":{\"type\":\"Struct\",\"fields\":[]}}";
    neogff::GffFile jsonGff;
    neogff::LoadGffXml(jsonGff, neogff::gffJsonToXml(json));
    require(jsonGff.root() != nullptr && jsonGff.root()->typeid_ == UINT32_MAX,
            "JSON import changed a missing root struct ID to zero");

    const std::string roundTripJson =
        neogff::gffXmlToJson("<gff3 type=\"UTC\"><struct/></gff3>");
    require(roundTripJson.find("4294967295") != std::string::npos,
            "XML-to-JSON conversion did not preserve the default root struct ID");
}

void loadVoidPayload(const std::string& payload) {
    neogff::GffFile gff;
    neogff::LoadGffXml(
        gff,
        "<gff3 type=\"UTC\"><struct><data label=\"Blob\">" + payload +
            "</data></struct></gff3>");
}


void testClassicTlkRoundTrip() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "neoshared-tlk-roundtrip.tlk";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    try {
        neotlk::TalkTable output;
        output.newFile();
        output.setLanguage(0);

        neotlk::TalkString entry;
        entry.flags = neotlk::TEXT_PRESENT | neotlk::SND_PRESENT;
        entry.soundResref = neotlk::ResRef::fromString("test_voice");
        entry.text = "Shared TLK round-trip: Café";
        output.addEntry(std::move(entry));
        output.save(path.string());

        const neotlk::TalkTable loaded(path.string());
        require(loaded.storageFormat() == neotlk::TlkStorageFormat::ClassicV30,
                "shared TLK parser changed the file format");
        require(loaded.count() == 1, "shared TLK parser changed the entry count");
        const neotlk::TalkString& loadedEntry = loaded.entries().front();
        require(loadedEntry.strRef == 0, "shared TLK parser changed the StrRef");
        require(loadedEntry.soundString() == "test_voice",
                "shared TLK parser changed the sound ResRef");
        require(loadedEntry.text == "Shared TLK round-trip: Café",
                "shared TLK parser changed the decoded text");

        neotlk::TlkLookup lookup;
        lookup.load(path);
        require(lookup.loaded(), "shared TLK lookup did not report a loaded table");
        require(lookup.languageId() == 0, "shared TLK lookup changed the language ID");
        require(lookup.count() == 1, "shared TLK lookup changed the entry count");
        require(lookup.resolve(0).value_or(std::string{}) ==
                    "Shared TLK round-trip: Café",
                "shared TLK lookup did not use the shared decoded text");
        require(!lookup.resolve(1).has_value(),
                "shared TLK lookup resolved an out-of-range StrRef");
    } catch (...) {
        std::filesystem::remove(path, ec);
        throw;
    }

    std::filesystem::remove(path, ec);
}


void testJadeTlkRoundTrip() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "neoshared-jade-tlk-roundtrip.tlk";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    try {
        neotlk::TalkTable output;
        output.newFile();
        output.setVersion40();
        output.setLanguage(0);

        neotlk::TalkString entry;
        entry.soundId = 42u;
        entry.text = "Shared Jade TLK round-trip — UTF-8";
        output.addEntry(std::move(entry));
        output.save(path.string());

        const neotlk::TalkTable loaded(path.string());
        require(loaded.storageFormat() == neotlk::TlkStorageFormat::JadeV40,
                "shared TLK parser changed the Jade file format");
        require(loaded.count() == 1, "shared Jade TLK parser changed the entry count");
        const neotlk::TalkString& loadedEntry = loaded.entries().front();
        require(loadedEntry.strRef == 0, "shared Jade TLK parser changed the StrRef");
        require(loadedEntry.soundId == 42u, "shared Jade TLK parser changed the sound ID");
        require(loadedEntry.text == "Shared Jade TLK round-trip — UTF-8",
                "shared Jade TLK parser changed the UTF-8 text");

        neotlk::TlkLookup lookup;
        lookup.load(path);
        require(lookup.resolve(0).value_or(std::string{}) ==
                    "Shared Jade TLK round-trip — UTF-8",
                "shared TLK lookup did not resolve Jade text");
    } catch (...) {
        std::filesystem::remove(path, ec);
        throw;
    }

    std::filesystem::remove(path, ec);
}

void testStrictBase64() {
    loadVoidPayload("TQ==");
    loadVoidPayload("TWE=");
    loadVoidPayload("TWFu");

    for (const std::string& malformed : {
             std::string("TQ=A"), std::string("TQ==TQ=="),
             std::string("TR=="), std::string("TWF=")}) {
        requireThrows([&] { loadVoidPayload(malformed); },
                      "malformed base64 payload was accepted: " + malformed);
    }
}

} // namespace

int main() {
    try {
        testGffResourceExtensions();
        testClassicGffV33RoundTrip();
        testDuplicateGffLabels();
        testDocumentDepthLimits();
        testMissingRootStructId();
        testClassicTlkRoundTrip();
        testJadeTlkRoundTrip();
        testStrictBase64();
        std::cout << "All shared format tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Shared format test failure: " << error.what() << '\n';
        return 1;
    }
}
