#pragma once

#include <string>
#include <string_view>

#include <qwr/fb2k_config.h>

namespace drp::config
{

enum class ImageSetting : uint8_t
{
    Light = 0,
    Dark,
    Disabled
};
enum class TimeSetting : uint8_t
{
    ProgressBar = 0,  // Shows progress bar (start + end timestamps)
    Elapsed = 1,      // Shows elapsed (start timestamp, old display mode)
    Disabled = 2      // Discord decides, i.e. elapsed since last status update, won't follow playback unless never interacted with
};
enum class StatusSetting : uint8_t
{
    Name = 0,
    Middle, // state / line 2; mildy confusing
    Top     // details / line 1
};
enum class UploaderMode : uint8_t
{
    Bundled = 0,
    CustomCommand
};
enum class BundledUploaderService : uint8_t
{
    Imgur = 0,
    Catbox
};

extern qwr::fb2k::ConfigBool isEnabled;
extern qwr::fb2k::ConfigUint8Enum<ImageSetting> largeImageSettings;
extern qwr::fb2k::ConfigUint8Enum<ImageSetting> smallImageSettings;
extern qwr::fb2k::ConfigUint8Enum<TimeSetting> timeSettings;
extern qwr::fb2k::ConfigUint8Enum<StatusSetting> statusSettings;
extern qwr::fb2k::ConfigString stateQuery;
extern qwr::fb2k::ConfigString detailsQuery;
extern qwr::fb2k::ConfigString largeImageTextQuery;

extern qwr::fb2k::ConfigString discordAppToken;
extern qwr::fb2k::ConfigString largeImageId_Light;
extern qwr::fb2k::ConfigString largeImageId_Dark;
extern qwr::fb2k::ConfigString playingImageId_Light;
extern qwr::fb2k::ConfigString playingImageId_Dark;
extern qwr::fb2k::ConfigString pausedImageId_Dark;
extern qwr::fb2k::ConfigString pausedImageId_Light;
extern qwr::fb2k::ConfigString uploadArtworkCommand;
extern qwr::fb2k::ConfigUint8Enum<UploaderMode> uploaderMode;
extern qwr::fb2k::ConfigUint8Enum<BundledUploaderService> bundledUploaderService;
extern qwr::fb2k::ConfigString bundledUploaderApiKey;
extern qwr::fb2k::ConfigBool uploaderPreferencesMigrated;
extern qwr::fb2k::ConfigString artworkMetadbKey;
extern qwr::fb2k::ConfigUint32 blurPercent;
extern qwr::fb2k::ConfigString hiddenPlaceholderUrl;
extern qwr::fb2k::ConfigUint32 processTimeout;
extern qwr::fb2k::ConfigBool overrideArtworkUrl;
extern qwr::fb2k::ConfigString artworkOverrideUrl;

extern qwr::fb2k::ConfigBool disableWhenPaused;
extern qwr::fb2k::ConfigBool swapSmallImages;
extern qwr::fb2k::ConfigBool uploadArtwork;
extern qwr::fb2k::ConfigBool autoUploadArtwork;

std::string TrimWhitespace( std::string_view raw );
void EnsureUploaderPreferencesMigrated();
UploaderMode GetValidatedUploaderMode();
BundledUploaderService GetValidatedBundledUploaderService();
std::string GetBundledUploaderServiceCliName();
uint32_t GetValidatedBlurPercent();
std::string GetValidatedHiddenPlaceholderUrl();
std::string GetValidatedArtworkOverrideUrl( metadb_handle_ptr metadb = metadb_handle_ptr() );

}; // namespace drp::config
