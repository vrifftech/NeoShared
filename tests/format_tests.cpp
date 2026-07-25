#include "GFFFile.hpp"
#include "GffJson.hpp"
#include "GffXml.hpp"
#include "SimpleJson.hpp"
#include "SimpleXml.hpp"
#include "neotlk/TlkFile.hpp"
#include "neotlk/TlkLookup.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
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
