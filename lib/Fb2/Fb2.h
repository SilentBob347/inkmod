#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Streaming FB2 -> unpacked EPUB-package converter. The generated package is
// consumed by Epub/EpubReaderActivity, so FB2 and EPUB share layout, images,
// TOC, footnotes, bookmarks, progress, reader settings, and navigation.
class Fb2 {
  enum class ParsePass : uint8_t { None, Scan, Render };

  struct ImageInfo {
    std::string id;
    std::string filename;
    std::string mediaType;
  };

  std::string filepath;
  std::string sourcePath;
  std::string temporarySourcePath;
  std::string cacheBaseDir;
  std::string cacheKey;
  std::string cachePath;
  std::string legacyCachePath;
  std::string packagePath;
  std::string title;
  std::string author;
  std::string language = "und";
  std::string coverImageId;
  uint64_t sourceSize = 0;
  bool loaded = false;

  XML_Parser parser = nullptr;
  ParsePass pass = ParsePass::None;
  HalFile output;
  HalFile tocRecords;
  HalFile anchorRecords;
  HalFile binaryOutput;

  int depth = 0;
  int titleInfoDepth = INT_MAX;
  int authorDepth = INT_MAX;
  int bodyDepth = INT_MAX;
  int titleElementDepth = INT_MAX;
  int binaryDepth = INT_MAX;
  int sectionLevel = 0;
  int sectionSerial = 0;
  int titleSerial = 0;
  int currentChapter = -1;
  int chapterCount = 0;
  int nextRenderChapter = 0;
  size_t chapterTextBytes = 0;
  int titleParagraphCount = 0;
  bool chapterOpen = false;
  bool inBookTitle = false;
  bool inFirstName = false;
  bool inMiddleName = false;
  bool inLastName = false;
  bool inNickname = false;
  bool inLanguage = false;
  bool inCoverpage = false;
  bool binaryWriteOk = true;
  bool binaryOutputOpen = false;

  std::string authorFirst;
  std::string authorMiddle;
  std::string authorLast;
  std::string authorNickname;
  std::string tocTitle;
  std::string activeBinaryPath;
  uint8_t tocLevel = 1;
  uint16_t tocChapter = 0;
  uint64_t tocAnchor = 0;
  std::vector<uint64_t> sectionAnchors;
  std::vector<ImageInfo> images;
  std::array<uint8_t, 4> base64Quartet{};
  uint8_t base64Count = 0;

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* text, int length);
  static int XMLCALL unknownEncoding(void* userData, const XML_Char* name, XML_Encoding* info);

  bool convertToPackage();
  bool prepareSource();
  bool isCompressedFb2() const;
  bool parseSource(ParsePass parsePass);
  bool cacheIsCurrent();
  bool loadMetadataCache();
  void saveMetadataCache() const;
  void saveCacheSignature() const;
  void maintainCacheBudget() const;
  void resetParserState(ParsePass parsePass);
  void finishAuthor();
  void postProcessMetadata();

  int ensureScanChapter();
  bool ensureRenderChapter();
  bool openChapter(int index);
  void closeChapter();
  void writeEscaped(const char* text, size_t length, bool attribute = false);
  void writeString(const std::string& value);
  void writeLiteral(const char* value);
  void writeElementId(const XML_Char** atts);

  void recordAnchor(const std::string& id, uint16_t chapter);
  bool findAnchorChapter(const std::string& id, uint16_t& chapter) const;
  void writeTocRecord();
  bool writeContainerFile() const;
  bool writeStyleFile() const;
  bool writeOpfFile() const;
  bool writeNcxFile() const;

  void beginBinary(const XML_Char** atts);
  void feedBase64(const char* text, size_t length);
  void endBinary();
  const ImageInfo* findImage(const std::string& id) const;

 public:
  explicit Fb2(std::string path, std::string cacheBasePath);
  ~Fb2();

  Fb2(const Fb2&) = delete;
  Fb2& operator=(const Fb2&) = delete;

  bool load();
  bool clearCache() const;
  void setupCacheDir() const;

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] const std::string& getPackagePath() const { return packagePath; }
  [[nodiscard]] const std::string& getTitle() const { return title; }
  [[nodiscard]] const std::string& getAuthor() const { return author; }
  [[nodiscard]] const std::string& getLanguage() const { return language; }
  [[nodiscard]] uint64_t getSourceSize() const { return sourceSize; }
  [[nodiscard]] int getChapterCount() const { return chapterCount; }
  [[nodiscard]] bool isLoaded() const { return loaded; }
};
