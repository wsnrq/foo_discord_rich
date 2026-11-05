#include <stdafx.h>

#include "uploader.h"

#include <fb2k/config.h>
#include <resource.h>

#include <span>
#include <chrono>
#include <ctime>
#include <vector>
#include <fb2k/artwork_metadb.h>

#include "discord_impl.h"
#include "image_hasher.h"
#include "foobar2000/SDK/component.h"

namespace drp::uploader
{
/**
 * Used to restrict uploading to one image at a time.
 * used by defining a variable of this type. Lock is automatically acquired,
 * and it is unlocked after the variable goes out of scope
 */
class upload_lock
{
public:
    upload_lock()
    {
        lock_.lock();
    }
    ~upload_lock()
    {
        lock_.unlock();
    }

    static bool is_locked()
    {
        const auto locked = lock_.try_lock();
        if ( locked )
        {
            lock_.unlock();
        }
        return !locked;
    }

private:
    static std::mutex lock_;
};

// Must initialize static class variables outside of the class
std::mutex upload_lock::lock_;

static bool hasArtworkData(const artwork_info& art);
static bool hasArtworkSource(const artwork_info& art);

static bool commandContainsPlaceholder( const std::string& commandString, const std::string& placeholder )
{
    return commandString.find( placeholder ) != std::string::npos;
}

static bool isCustomCommandMode()
{
    return config::GetValidatedUploaderMode() == config::UploaderMode::CustomCommand;
}

static bool validateBlurredUploadEnabled( const UploadOptions& options )
{
    if ( options.mode != ArtworkMode::Blurred )
    {
        return true;
    }

    if ( options.blurPercent == 0 )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Blurred artwork is unavailable because Blur Strength is set to 0";
        return false;
    }

    return true;
}

static bool validateCustomCommandBlurredUploadSupport( const UploadOptions& options, const std::string& commandString )
{
    if ( !validateBlurredUploadEnabled( options ) )
    {
        return false;
    }

    if ( options.mode != ArtworkMode::Blurred )
    {
        return true;
    }

    if ( commandString.empty() || commandString.find_first_not_of( " \t\n\r" ) == std::string::npos )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Blurred artwork is unavailable because no upload command is configured";
        return false;
    }

    const bool hasFilePathPlaceholder = commandContainsPlaceholder( commandString, "{filepath}" );
    const bool hasBlurPercentPlaceholder = commandContainsPlaceholder( commandString, "{blur_percent}" );

    if ( !hasFilePathPlaceholder || !hasBlurPercentPlaceholder )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Blurred artwork is unavailable because the uploader command must contain both {filepath} and {blur_percent}";
        return false;
    }

    return true;
}

static bool validateConfiguredUploadSupport( const UploadOptions& options )
{
    if ( !validateBlurredUploadEnabled( options ) )
    {
        return false;
    }

    if ( isCustomCommandMode() )
    {
        return validateCustomCommandBlurredUploadSupport( options, config::uploadArtworkCommand.GetValue() );
    }

    return true;
}

static bool currentPlaybackMatchesAnyHash( const pfc::avltree_t<metadb_index_hash>& hashes )
{
    if ( hashes.get_count() == 0 )
    {
        return false;
    }

    metadb_handle_ptr currentTrack;
    if ( !playback_control::get()->get_now_playing( currentTrack ) )
    {
        return false;
    }

    metadb_index_hash currentHash;
    if ( !clientByGUID( guid::artwork_url_index )->hashHandle( currentTrack, currentHash ) )
    {
        return false;
    }

    return hashes.contains( currentHash );
}

static UploadOptions makeUploadOptions( ArtworkMode mode, bool regenerate )
{
    UploadOptions options;
    options.mode = mode;
    options.regenerate = regenerate;
    options.allowCachedReuse = !regenerate;
    options.allowUrlPlaceholderReuse = ( mode == ArtworkMode::Normal && !regenerate );
    if ( mode == ArtworkMode::Blurred )
    {
        options.blurPercent = config::GetValidatedBlurPercent();
    }

    return options;
}

static pfc::string8 getCanonicalArtworkUrl( metadb_index_hash hash )
{
    if ( hash == metadb_index_hash() )
    {
        return {};
    }

    const auto rec = record_get( hash );
    if ( rec.artwork_url.get_length() > 0 && isValidUrl( rec.artwork_url ) )
    {
        return rec.artwork_url;
    }

    return {};
}

static pfc::string8 getModeScopedCachedUrl( artwork_info& art,
                                            abort_callback& abort,
                                            metadb_index_hash hash,
                                            const UploadOptions& options )
{
    if ( !options.allowCachedReuse )
    {
        return {};
    }

    if ( options.mode == ArtworkMode::Normal )
    {
        auto canonicalUrl = getCanonicalArtworkUrl( hash );
        if ( canonicalUrl.get_length() > 0 )
        {
            return canonicalUrl;
        }

        if ( hasArtworkData( art ) )
        {
            pfc::string8 hashUrl;
            check_artwork_hash( art, abort, hashUrl, ArtworkMode::Normal, 0 );
            if ( hashUrl.get_length() > 0 && isValidUrl( hashUrl ) )
            {
                return hashUrl;
            }
        }
    }
    else if ( options.mode == ArtworkMode::Blurred && hasArtworkData( art ) )
    {
        pfc::string8 hashUrl;
        check_artwork_hash( art, abort, hashUrl, ArtworkMode::Blurred, options.blurPercent );
        if ( hashUrl.get_length() > 0 && isValidUrl( hashUrl ) )
        {
            return hashUrl;
        }
    }

    return {};
}

static pfc::string8 getModeScopedPlaceholderUrl( artwork_info& art,
                                                 abort_callback& abort,
                                                 metadb_index_hash hash,
                                                 const UploadOptions& options )
{
    if ( !options.allowUrlPlaceholderReuse )
    {
        return {};
    }

    if ( options.mode == ArtworkMode::Normal )
    {
        return getCanonicalArtworkUrl( hash );
    }

    if ( options.mode == ArtworkMode::Blurred && hasArtworkData( art ) )
    {
        pfc::string8 hashUrl;
        check_artwork_hash( art, abort, hashUrl, ArtworkMode::Blurred, options.blurPercent );
        if ( hashUrl.get_length() > 0 && isValidUrl( hashUrl ) )
        {
            return hashUrl;
        }
    }

    return {};
}

static bool hasArtworkData(const artwork_info& art)
{
    return art.data.is_valid();
}

static bool hasArtworkSource(const artwork_info& art)
{
    return strlen(art.path) > 0 || hasArtworkData(art);
}

static void ensureArtworkHash(artwork_info& art)
{
    if (art.artwork_hash.get_length() > 0 || !hasArtworkData(art))
    {
        return;
    }

    art.artwork_hash = static_api_ptr_t<hasher_md5>()
        ->process_single(art.data->get_ptr(), art.data->get_size())
        .asString();
}

static bool tryGetDirectArtworkFilepath(const pfc::string8& rawPath, pfc::string8& filepath)
{
    if (rawPath.get_length() == 0)
    {
        return false;
    }

    filepath = rawPath;
    if (filepath.has_prefix("file://"))
    {
        // Remove file:// protocol since the program expects a file path instead of a uri
        if (filepath.replace_string("file://", "") > 1)
        {
            // This should never really be reached
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Multiple instances of \"file://\" replaced during cover upload";
        }

        return true;
    }

    return strstr(filepath.c_str(), "://") == nullptr;
}

static bool materializeArtworkTempFile(const artwork_info& art,
                                       abort_callback& abort,
                                       pfc::string8& filepath,
                                       pfc::string8& tempFile,
                                       bool& deleteFile)
{
    if (!hasArtworkData(art))
    {
        return false;
    }

    auto tempDir = core_api::pathInProfile(DRP_UNDERSCORE_NAME);

    if (!filesystem::g_exists(tempDir.c_str(), abort))
    {
        filesystem::g_create_directory(tempDir.c_str(), abort);
    }

    // Get the image mime type since we cannot deduce it otherwise from embedded artwork
    const auto api = fb2k::imageLoaderLite::tryGet();
    std::string ext = "jpg";
    if (api.is_valid())
    {
        const auto info = api->getInfo(art.data->get_ptr(), art.data->get_size(), abort);
        abort.check();

        const auto mime = std::string(info.mime);
        // Use mime type extension for image/ mime types. Not perfect and will fail for some of the more exotic types like svg
        if (mime.rfind("image/", 0) == 0)
        {
            ext = mime.substr(mime.find("/") + 1);
        }
    }

    // Last 10 digits of current time for filename.
    const auto ts = std::to_string(std::time(NULL));
    const auto filename = pfc::string8((ts.substr(std::max(ts.size(), (size_t)10) - 10) + "." + ext).c_str());

    tempDir.add_filename(filename.c_str());
    const auto rawTempPath = tempDir;
    deleteFile = true;

    #ifdef _DEBUG
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": full temp filepath " << rawTempPath;
    #endif

    // Keep the raw foobar path for file operations, but expose a direct local path to the external uploader.
    tempFile = rawTempPath;
    {
        service_ptr_t<file> file_ptr;
        // File gets released after file_ptr has been deleted
        filesystem::g_open_write_new( file_ptr, tempFile, abort );
        file_ptr->write( art.data->get_ptr(), art.data->get_size(), abort );
    }

    if (tryGetDirectArtworkFilepath(rawTempPath, filepath))
    {
        return true;
    }

    filepath = rawTempPath;
    return true;
}

threaded_process_artwork_uploader::threaded_process_artwork_uploader(
    const pfc::map_t<metadb_index_hash, metadb_handle_ptr>& hashes, const bool regenerate) : hashes_(hashes), regenerate_(regenerate)
{}

void threaded_process_artwork_uploader::on_init(ctx_t p_wnd) {}
    
void threaded_process_artwork_uploader::run(threaded_process_status &p_status, abort_callback &p_abort)
{
    pfc::list_t<metadb_index_hash> lstChanged; // Linear list of hashes that actually changed
    pfc::avltree_t<metadb_index_hash> succeededHashes;
    size_t successCount = 0;
    size_t skippedHiddenCount = 0;
    size_t failedCount = 0;
    const auto total_count = hashes_.get_count();
    t_size currIdx = 0;
    
    for (auto iter = hashes_.first(); iter.is_valid(); ++iter) {
        try
        {
            p_status.set_progress_float( currIdx / (double)total_count );
            const auto kv = *iter;

            const auto rec = record_get( kv.m_key );
            if ( rec.artwork_mode == ArtworkMode::Hidden )
            {
                ++skippedHiddenCount;
                p_abort.check();
                currIdx++;
                continue;
            }

            if ( !regenerate_
                 && rec.artwork_mode == ArtworkMode::Normal
                 && rec.artwork_url.get_length() > 0
                 && isValidUrl( rec.artwork_url ) )
            {
                p_abort.check();
                currIdx++;
                continue;
            }

            const auto options = makeUploadOptions( rec.artwork_mode, regenerate_ );
            pfc::string8 artwork_url;
            bool recordChanged = false;

            if ( extractAndUploadArtwork( kv.m_value, p_abort, artwork_url, kv.m_key, options, &recordChanged ) )
            {
                ++successCount;
                if ( !succeededHashes.contains( kv.m_key ) )
                {
                    succeededHashes += kv.m_key;
                }
                if ( recordChanged )
                {
                    lstChanged += kv.m_key;
                }
            }
            else
            {
                ++failedCount;
            }
            
            p_abort.check();
        }
        catch (exception_aborted)
        {
            return;
        }
        currIdx++;
    }

    p_status.set_progress_float(1);
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Processed artwork for " << successCount
                             << " group(s), failed " << failedCount << " group(s)";
    if ( skippedHiddenCount > 0 )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Skipped " << skippedHiddenCount
                                 << " hidden artwork group(s)";
    }


    if ( lstChanged.get_count() > 0 || successCount > 0 )
    {
        fb2k::inMainThread([lstChanged, succeededHashes]
        {
            if ( lstChanged.get_count() > 0 )
            {
                // This gracefully tells everyone about what just changed, in one pass regardless of how many items got altered
                cached_index_api()->dispatch_refresh(guid::artwork_url_index, lstChanged);
            }
            if ( currentPlaybackMatchesAnyHash( succeededHashes ) )
            {
                DiscordHandler::GetInstance().GetPresenceModifier().UpdateImage();
            }
        });
    }
}

void threaded_process_artwork_uploader::on_done(ctx_t p_wnd,bool p_was_aborted)
{
}

bool extractAndUploadArtwork( const metadb_handle_ptr track,
                              abort_callback &abort,
                              pfc::string8 &artwork_url,
                              metadb_index_hash hash,
                              const UploadOptions& options,
                              bool* recordChanged )
{
    if ( recordChanged )
    {
        *recordChanged = false;
    }

    if ( !validateConfiguredUploadSupport( options ) )
    {
        return false;
    }

    upload_lock lock;
    abort.check();

    auto artwork = extractArtwork( track, abort );
    abort.check();
    if (artwork.success)
    {
#ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Artwork path after extract " << artwork.path;
#endif
        artwork_url = uploadArtwork( artwork, abort, options, hash );
        if (artwork_url.get_length() > 0)
        {
            bool changed = false;
            if ( options.mode == ArtworkMode::Normal )
            {
                changed = drp::artwork_url_set( hash, artwork_url );
            }
            else if ( options.mode == ArtworkMode::Blurred )
            {
                changed = drp::blurred_artwork_url_set( hash, artwork_url, options.blurPercent, artwork.artwork_hash );
            }

            if ( recordChanged )
            {
                *recordChanged = changed;
            }
            return true;
        }
    }
    else if ( isCustomCommandMode()
              && options.mode == ArtworkMode::Normal
              && options.allowUrlPlaceholderReuse
              && usesUrlPlaceholder( config::uploadArtworkCommand.GetValue() ) )
    {
        // Artwork extraction failed but command uses {url} placeholder, so proceed with uploadArtwork
        artwork_url = uploadArtwork( artwork, abort, options, hash );
        if (artwork_url.get_length() > 0)
        {
            const bool changed = drp::artwork_url_set( hash, artwork_url );
            if ( recordChanged )
            {
                *recordChanged = changed;
            }
            return true;
        }
    }

    return false;
}

artwork_info extractArtwork( const metadb_handle_ptr track, abort_callback &abort )
{
    auto aam = album_art_manager_v3::get();
    auto extractor = aam->open(pfc::list_single_ref_t(track),
                           pfc::list_single_ref_t(album_art_ids::cover_front), abort);

    if (!extractor.is_valid())
    {
        #ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Invalid artwork extractor instance found";
        #endif
        return artwork_info();
    }

    try {
        abort.check();
        artwork_info info;

        const auto paths = extractor->query_paths( album_art_ids::cover_front, abort );
        if (paths->get_count() > 0)
        {
            info.path = pfc::string8(paths->get_path(0));
            auto loc = track->get_location().get_path();

            #ifdef _DEBUG
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": extracted filepath " << info.path << " track filepath " << loc << " is valid? " << paths.is_valid();
            #endif
            // Artwork location same as file means artwork is embedded
            if (info.path.equals(loc))
            {
                info.path = "";
            }
        }
        else
        {
            info.path = "";
        }
        
        info.data = extractor->query(album_art_ids::cover_front, abort);
        info.success = true;

        #ifdef _DEBUG
        if (info.path != "")
        {
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": found existing path " << info.path;
        }
        #endif
        return info;
    } catch (const exception_album_art_not_found&) {
        return artwork_info();
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        return artwork_info();
    }
}

/**
 * Initialize different process variables to be used with CreateProcess
 */
bool initializeProcessVariables(
    HANDLE &g_hChildStd_IN_Rd,
    HANDLE &g_hChildStd_IN_Wr,
    HANDLE &g_hChildStd_OUT_Rd,
    HANDLE &g_hChildStd_OUT_Wr,
    SECURITY_ATTRIBUTES &saAttr)
{
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if ( !CreatePipe( &g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0 ) )
        return false;

    // Ensure the read handle to the pipe for STDOUT is not inherited.

    if ( !SetHandleInformation( g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0 ) )
        return false;

    // Create a pipe for the child process's STDIN.

    if ( !CreatePipe( &g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0 ) )
        return false;

    // Ensure the write handle to the pipe for STDIN is not inherited.

    if ( !SetHandleInformation( g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0 ) )
        return false;

    return true;
}

void closePipes(HANDLE pipe1, HANDLE pipe2, HANDLE pipe3, HANDLE pipe4)
{
    if (pipe1 != NULL) CloseHandle(pipe1);
    if (pipe2 != NULL) CloseHandle(pipe2);
    if (pipe3 != NULL) CloseHandle(pipe3);
    if (pipe4 != NULL) CloseHandle(pipe4);
}

bool readFromPipe(HANDLE g_hChildStd_OUT_Rd,
                  pfc::string8 &artwork_url,
                  std::chrono::seconds timeout)
{
    bool inputFound = false;
    const int TEMP_BUF_SIZE = 16;
    CHAR tempBuf[TEMP_BUF_SIZE];
    DWORD peekedBytes;
    const auto now = std::chrono::high_resolution_clock::now();

    // Try to read output from the process until the configured timeout expires.
    while (!inputFound)
    {
        if (PeekNamedPipe( g_hChildStd_OUT_Rd, tempBuf, TEMP_BUF_SIZE, &peekedBytes, NULL, NULL ))
        {
            if (peekedBytes > 0)
            {
                inputFound = true;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto td = std::chrono::high_resolution_clock::now() - now;
        if (td > timeout) break;
    }

    bool rSuccess = false;
    if (inputFound)
    {
        DWORD dwRead;
        // Should be good enough amount of characters
        const int BUFSIZE = 2048;
        CHAR chBuf[BUFSIZE];
        rSuccess = ReadFile( g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL );
        artwork_url = pfc::string8(chBuf, dwRead);
        // rtrim space like characters
        artwork_url.skip_trailing_chars(" \t\n\r");
    }

    return rSuccess;
}

bool getArtworkFilepath(const artwork_info& art,
                        abort_callback& abort,
                        pfc::string8& filepath,
                        pfc::string8& tempFile,
                        bool& deleteFile)
{
    abort.check();

    // Tempfile assigned later to make sure the actual cover art is not deleted after uploading
    filepath = "";
    tempFile = "";
    deleteFile = false;

    if (strlen(art.path) > 0)
    {
        pfc::string8 directPath;
        if (tryGetDirectArtworkFilepath(art.path, directPath))
        {
            if (filesystem::g_exists(directPath.c_str(), abort))
            {
                filepath = directPath;
                return true;
            }

            FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                     << ": Artwork path is not accessible, falling back to extracted bytes: "
                                     << directPath;
        }
        else
        {
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                     << ": Artwork path is not a direct local file, falling back to extracted bytes: "
                                     << art.path;
        }
    }

    if (materializeArtworkTempFile(art, abort, filepath, tempFile, deleteFile))
    {
        return true;
    }

    if (strlen(art.path) > 0)
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Could not resolve a usable local artwork file for upload: "
                                 << art.path;
    }
    else
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Could not materialize artwork data for upload";
    }

    return false;
}

struct BundledUploaderBinary
{
    int resourceId;
    const char* archName;
    const char* filename;
};

static constexpr BundledUploaderBinary bundledUploaderX86{
    IDR_BUNDLED_ARTWORK_UPLOADER_X86,
    "x86",
    "drp_artwork_uploader_windows_386.exe",
};

static constexpr BundledUploaderBinary bundledUploaderX64{
    IDR_BUNDLED_ARTWORK_UPLOADER_X64,
    "x64",
    "drp_artwork_uploader_windows_amd64.exe",
};

static bool tryDetectNativeMachineWithIsWow64Process2( USHORT& nativeMachine )
{
    nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;

    using IsWow64Process2Proc = BOOL( WINAPI* )( HANDLE, USHORT*, USHORT* );

    const HMODULE kernel32 = GetModuleHandleW( L"kernel32.dll" );
    if ( !kernel32 )
    {
        return false;
    }

    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Proc>( GetProcAddress( kernel32, "IsWow64Process2" ) );
    if ( !isWow64Process2 )
    {
        return false;
    }

    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT detectedNativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if ( !isWow64Process2( GetCurrentProcess(), &processMachine, &detectedNativeMachine ) )
    {
        return false;
    }

    if ( detectedNativeMachine == IMAGE_FILE_MACHINE_UNKNOWN )
    {
        return false;
    }

    nativeMachine = detectedNativeMachine;
    return true;
}

static USHORT getNativeMachineFromSystemInfo()
{
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo( &systemInfo );

    switch ( systemInfo.wProcessorArchitecture )
    {
    case PROCESSOR_ARCHITECTURE_INTEL:
        return IMAGE_FILE_MACHINE_I386;
    case PROCESSOR_ARCHITECTURE_AMD64:
        return IMAGE_FILE_MACHINE_AMD64;
#ifdef PROCESSOR_ARCHITECTURE_ARM64
    case PROCESSOR_ARCHITECTURE_ARM64:
#ifdef IMAGE_FILE_MACHINE_ARM64
        return IMAGE_FILE_MACHINE_ARM64;
#else
        return 0xaa64;
#endif
#endif
    default:
        return IMAGE_FILE_MACHINE_UNKNOWN;
    }
}

static USHORT detectNativeMachine()
{
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if ( tryDetectNativeMachineWithIsWow64Process2( nativeMachine ) )
    {
        return nativeMachine;
    }

    return getNativeMachineFromSystemInfo();
}

static std::string getNativeMachineDisplay( const USHORT machine )
{
    switch ( machine )
    {
    case IMAGE_FILE_MACHINE_I386:
        return "x86";
    case IMAGE_FILE_MACHINE_AMD64:
        return "x64";
#ifdef IMAGE_FILE_MACHINE_ARM64
    case IMAGE_FILE_MACHINE_ARM64:
        return "ARM64";
#endif
    case IMAGE_FILE_MACHINE_UNKNOWN:
        return "unknown";
    default:
        return fmt::format( "unknown PE machine 0x{:04x}", machine );
    }
}

static void logUnsupportedBundledUploaderArchitectureOnce( const USHORT nativeMachine )
{
    static std::mutex logMutex;
    static bool logged = false;

    std::lock_guard lock( logMutex );
    if ( logged )
    {
        return;
    }

    if ( nativeMachine == IMAGE_FILE_MACHINE_UNKNOWN )
    {
        FB2K_console_formatter()
            << DRP_NAME_WITH_VERSION
            << ": Bundled artwork uploader supports only x86/x64 native architectures; native architecture could not be detected. Custom command uploader mode remains available.";
    }
    else
    {
        const auto nativeMachineDisplay = getNativeMachineDisplay( nativeMachine );
        FB2K_console_formatter()
            << DRP_NAME_WITH_VERSION
            << ": Bundled artwork uploader supports only x86/x64 native architectures; detected native architecture is "
            << nativeMachineDisplay.c_str()
            << ". Custom command uploader mode remains available.";
    }

    logged = true;
}

static const BundledUploaderBinary* selectBundledUploaderBinary()
{
    const USHORT nativeMachine = detectNativeMachine();
    switch ( nativeMachine )
    {
    case IMAGE_FILE_MACHINE_I386:
        return &bundledUploaderX86;
    case IMAGE_FILE_MACHINE_AMD64:
        return &bundledUploaderX64;
    default:
        logUnsupportedBundledUploaderArchitectureOnce( nativeMachine );
        return nullptr;
    }
}

static std::string getBundledUploaderResourceDescription( const BundledUploaderBinary& binary )
{
    return std::string( binary.archName ) + " resource ID " + std::to_string( binary.resourceId );
}

static std::span<const uint8_t> getBundledUploaderResourceBytes( const BundledUploaderBinary& binary )
{
    const HRSRC resource = FindResourceW( core_api::get_my_instance(),
                                          MAKEINTRESOURCEW( binary.resourceId ),
                                          RT_RCDATA );
    const auto resourceDescription = getBundledUploaderResourceDescription( binary );
    if ( !resource )
    {
        throw std::runtime_error( "Bundled uploader " + resourceDescription + " was not found" );
    }

    const DWORD resourceSize = SizeofResource( core_api::get_my_instance(), resource );
    if ( resourceSize == 0 )
    {
        throw std::runtime_error( "Bundled uploader " + resourceDescription + " is empty" );
    }

    const HGLOBAL resourceData = LoadResource( core_api::get_my_instance(), resource );
    if ( !resourceData )
    {
        throw std::runtime_error( "Bundled uploader " + resourceDescription + " could not be loaded" );
    }

    const auto* bytes = static_cast<const uint8_t*>( LockResource( resourceData ) );
    if ( !bytes )
    {
        throw std::runtime_error( "Bundled uploader " + resourceDescription + " could not be locked" );
    }

    return { bytes, static_cast<size_t>( resourceSize ) };
}

static pfc::string8 getBundledUploaderDirectory()
{
    auto path = core_api::pathInProfile( DRP_UNDERSCORE_NAME );
    path.add_filename( "bundled_uploader" );
    return path;
}

static pfc::string8 getBundledUploaderExecutablePath( const BundledUploaderBinary& binary )
{
    auto path = getBundledUploaderDirectory();
    path.add_filename( binary.filename );
    return path;
}

static bool bundledUploaderFileMatchesResource( const pfc::string8& path,
                                                std::span<const uint8_t> resourceBytes,
                                                abort_callback& abort )
{
    if ( !filesystem::g_exists( path.c_str(), abort ) )
    {
        return false;
    }

    try
    {
        service_ptr_t<file> existingFile;
        filesystem::g_open_read( existingFile, path.c_str(), abort );

        const auto existingSize = existingFile->get_size_ex( abort );
        if ( existingSize != static_cast<t_filesize>( resourceBytes.size() ) )
        {
            return false;
        }

        std::vector<uint8_t> existingBytes( resourceBytes.size() );
        existingFile->read_object( existingBytes.data(), existingBytes.size(), abort );

        return std::equal( resourceBytes.begin(), resourceBytes.end(), existingBytes.cbegin(), existingBytes.cend() );
    }
    catch ( const exception_aborted& )
    {
        throw;
    }
    catch ( ... )
    {
        return false;
    }
}

static void extractBundledUploaderExecutable( const pfc::string8& path,
                                              std::span<const uint8_t> resourceBytes,
                                              abort_callback& abort )
{
    if ( filesystem::g_exists( path.c_str(), abort ) )
    {
        filesystem::g_remove( path.c_str(), abort );
    }

    service_ptr_t<file> outputFile;
    filesystem::g_open_write_new( outputFile, path.c_str(), abort );
    outputFile->write( resourceBytes.data(), resourceBytes.size(), abort );
}

static bool& getBundledUploaderVerifiedFlag( const BundledUploaderBinary& binary )
{
    static bool x86Verified = false;
    static bool x64Verified = false;

    return binary.resourceId == IDR_BUNDLED_ARTWORK_UPLOADER_X64 ? x64Verified : x86Verified;
}

static bool ensureBundledUploaderReady( abort_callback& abort,
                                        pfc::string8& executablePath,
                                        const BundledUploaderBinary*& selectedBinary )
{
    static std::mutex verificationMutex;

    selectedBinary = selectBundledUploaderBinary();
    if ( !selectedBinary )
    {
        executablePath = "";
        return false;
    }

    const auto targetPath = getBundledUploaderExecutablePath( *selectedBinary );
    pfc::string8 directPath;
    executablePath = tryGetDirectArtworkFilepath( targetPath, directPath ) ? directPath : targetPath;

    std::lock_guard lock( verificationMutex );
    bool& verified = getBundledUploaderVerifiedFlag( *selectedBinary );
    if ( verified )
    {
        return true;
    }

    try
    {
        const auto resourceBytes = getBundledUploaderResourceBytes( *selectedBinary );
        const auto targetDir = getBundledUploaderDirectory();
        if ( !filesystem::g_exists( targetDir.c_str(), abort ) )
        {
            filesystem::g_create_directory( targetDir.c_str(), abort );
        }

        if ( !bundledUploaderFileMatchesResource( targetPath, resourceBytes, abort ) )
        {
            extractBundledUploaderExecutable( targetPath, resourceBytes, abort );
        }

        verified = true;
#ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Using bundled artwork uploader architecture "
                                 << selectedBinary->archName;
#endif
        return true;
    }
    catch ( const exception_aborted& )
    {
        throw;
    }
    catch ( const std::exception& e )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Failed to prepare bundled artwork uploader ("
                                 << selectedBinary->archName
                                 << ", resource ID " << selectedBinary->resourceId
                                 << ", target '" << targetPath
                                 << "'): " << e.what();
        return false;
    }
    catch ( ... )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Failed to prepare bundled artwork uploader ("
                                 << selectedBinary->archName
                                 << ", resource ID " << selectedBinary->resourceId
                                 << ", target '" << targetPath
                                 << "')";
        return false;
    }
}

std::wstring to_wstring(const std::string &str)
{
    // Just check size
    int convertResult = MultiByteToWideChar(
            CP_UTF8,
            0,
            str.c_str(),
            (DWORD)str.length(),
            NULL,
            0);

    if ( convertResult <= 0 )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Failed to convert command to utf-16. Error code " << convertResult;
        throw std::invalid_argument( "Could not convert command to utf-16" );
    }

    std::wstring str_w;
    // https://docs.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar
    // The example has + 10 without any comment as to why
    str_w.resize( convertResult + 10 );

    // This one writes the bytes to str_w
    convertResult = MultiByteToWideChar(
        CP_UTF8,
        0,
        str.c_str(),
        (DWORD)str.length(),
        &str_w[0],
        (int)str_w.size() );

    if ( convertResult <= 0 )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Failed to convert command to utf-16. Error code " << convertResult;
        throw std::invalid_argument( "Could not convert command to utf-16" );
    }

    return str_w;
}

/*
    The previous implementation would use stdin, we're keeping it for backwards compatibility with older scripts.
    When placeholder string {filepath} is found, it'll substitute it and pass it as a positional argument, skipping stdin.
    TODO: Add current URL with {cachedurl}, this would allow a script to verify it's still up and skip it OR pass it back (do we want that?)
*/
std::pair<std::wstring, std::wstring> parseCommand(const std::string &commandString,
                                                   const std::string &filepath = "",
                                                   const std::string &url = "",
                                                   const std::string &blurPercent = "")
{
    auto quoteIfNeeded = []( const std::string& value ) -> std::string {
        if ( value.empty() || value.find( ' ' ) == std::string::npos || value[0] == '"' )
        {
            return value;
        }

        return "\"" + value + "\"";
    };
    auto substitutePlaceholder = [quoteIfNeeded]( std::string& command,
                                                  const std::string& placeholder,
                                                  const std::string& value,
                                                  const bool autoQuote ) {
        size_t placeholderPos = 0;
        while ( ( placeholderPos = command.find( placeholder, placeholderPos ) ) != std::string::npos )
        {
            const size_t placeholderEnd = placeholderPos + placeholder.length();
            const bool isAlreadyQuoted = ( placeholderPos > 0 && placeholderEnd < command.size()
                                           && command[placeholderPos - 1] == '"'
                                           && command[placeholderEnd] == '"' );

            const auto replacement = ( autoQuote && !isAlreadyQuoted )
                                         ? quoteIfNeeded( value )
                                         : value;

            command.replace( placeholderPos, placeholder.length(), replacement );
            placeholderPos += replacement.length();
        }
    };

    std::string processedCommand = commandString;

    substitutePlaceholder( processedCommand, "{filepath}", filepath, true );
    substitutePlaceholder( processedCommand, "{url}", url, false );
    substitutePlaceholder( processedCommand, "{blur_percent}", blurPercent, false );

    std::wstring cmd_w = to_wstring(processedCommand);
    std::wstring executable;
    std::wstring fullCmdLine = cmd_w;

    // Find the executable part (first token, handling quotes)
    size_t pos = 0;
    if (cmd_w[0] == L'"') {
        // Quoted executable path
        pos = cmd_w.find(L'"', 1);
        if (pos != std::wstring::npos) {
            executable = cmd_w.substr(1, pos - 1); // remove quotes
            pos++; // move past the closing quote
        }
    } else {
        // Unquoted executable path - find first space
        pos = cmd_w.find(L' ');
        if (pos != std::wstring::npos) {
            executable = cmd_w.substr(0, pos);
        } else {
            executable = cmd_w; // no arguments
        }
    }

    return std::make_pair(executable, fullCmdLine);
}

/*
    Check if command uses file path placeholder
*/
bool usesFilePathPlaceholder(const std::string &commandString)
{
    return commandContainsPlaceholder( commandString, "{filepath}" );
}

/*
    Validate command string
*/
bool validateCommandString(const std::string &commandString, std::string &errorMessage)
{
    // Check for empty/whitespace-only command
    if (commandString.empty() || commandString.find_first_not_of(" \t\n\r") == std::string::npos) {
        errorMessage = "Invalid command string: is empty or only contains whitespaces.";
        return false;
    }

    // Basic quote matching check
    size_t quoteCount = 0;
    for (char c : commandString) {
        if (c == '"') quoteCount++;
    }
    if (quoteCount % 2 != 0) {
        errorMessage = "Invalid command string: quotes do not match.";
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Warning: " << errorMessage;
        // Keep going, receiver might handle it fine..?
        // Consider adding a popup warning for certain ones?
    }

    return true;
}

/*
    Get Windows error description from error code
*/
std::string getWindowsErrorDescription(DWORD errorCode)
{
    LPSTR messageBuffer = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&messageBuffer, 0, NULL);

    if (size && messageBuffer) {
        std::string message(messageBuffer, size);
        LocalFree(messageBuffer);

        // Remove trailing return or newline
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
            message.pop_back();
        }
        return message;
    }

    return "Unknown error";
}

std::string getWindowsErrorName( DWORD errorCode )
{
    switch ( errorCode )
    {
    case ERROR_FILE_NOT_FOUND:
        return "ERROR_FILE_NOT_FOUND";
    case ERROR_PATH_NOT_FOUND:
        return "ERROR_PATH_NOT_FOUND";
    case ERROR_ACCESS_DENIED:
        return "ERROR_ACCESS_DENIED";
    case ERROR_BAD_EXE_FORMAT:
        return "ERROR_BAD_EXE_FORMAT";
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
        return "ERROR_EXE_MACHINE_TYPE_MISMATCH";
    case ERROR_WAIT_1:
        return "ERROR_WAIT_1";
    default:
        return "ERROR_" + std::to_string( errorCode );
    }
}

std::string wideToUtf8( const std::wstring& value )
{
    if ( value.empty() )
    {
        return {};
    }

    const int size = WideCharToMultiByte( CP_UTF8,
                                          0,
                                          value.c_str(),
                                          -1,
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr );
    if ( size <= 0 )
    {
        return {};
    }

    std::string result( static_cast<size_t>( size - 1 ), '\0' );
    WideCharToMultiByte( CP_UTF8,
                         0,
                         value.c_str(),
                         -1,
                         result.data(),
                         size,
                         nullptr,
                         nullptr );
    return result;
}

std::string getFileSizeDisplay( const std::wstring& path )
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if ( !GetFileAttributesExW( path.c_str(), GetFileExInfoStandard, &data ) )
    {
        return "unavailable";
    }

    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return std::to_string( size.QuadPart ) + " bytes";
}

std::string getBinaryTypeDisplay( const std::wstring& path )
{
    DWORD binaryType = 0;
    if ( !GetBinaryTypeW( path.c_str(), &binaryType ) )
    {
        return "unavailable (" + getWindowsErrorName( GetLastError() ) + ")";
    }

    switch ( binaryType )
    {
    case SCS_32BIT_BINARY:
        return "32-bit Windows";
    case SCS_64BIT_BINARY:
        return "64-bit Windows";
    case SCS_DOS_BINARY:
        return "DOS";
    case SCS_OS216_BINARY:
        return "16-bit OS/2";
    case SCS_PIF_BINARY:
        return "PIF";
    case SCS_POSIX_BINARY:
        return "POSIX";
    case SCS_WOW_BINARY:
        return "16-bit Windows";
    default:
        return "unknown (" + std::to_string( binaryType ) + ")";
    }
}

/*
    Simple validation, does it look like a normal URL? Insert <falsehoods people believe about URLs> here
*/
bool isValidUrl(const pfc::string8 &url)
{
    if (url.get_length() == 0) {
        return false;
    }

    // Check for basic URL patterns
    const char* url_cstr = url.c_str();
    return (strncmp(url_cstr, "http://", 7) == 0 ||
            strncmp(url_cstr, "https://", 8) == 0);
}

bool uploadOpenProcess(const std::wstring &executable, const std::wstring &cmd_w, const char* filepath_c, pfc::string8 &artwork_url,
    STARTUPINFO &siStartInfo, PROCESS_INFORMATION &piProcInfo,
    HANDLE &g_hChildStd_OUT_Wr, HANDLE &g_hChildStd_IN_Rd, HANDLE &g_hChildStd_IN_Wr, HANDLE &g_hChildStd_OUT_Rd,
    bool useStdinForFilepath, std::chrono::seconds timeout, const char* uploaderLabel)
{
     // Create the child process
     std::wstring mutableCmdLine = cmd_w; // https://devblogs.microsoft.com/oldnewthing/20090601-00/?p=18083
     bool bSuccess = CreateProcessW(
        executable.empty() ? NULL : executable.c_str(), // application name
        (LPWSTR)mutableCmdLine.c_str(), // command line
        NULL,          // process security attributes
        NULL,          // primary thread security attributes
        TRUE,          // handles are inherited
        CREATE_NO_WINDOW, // creation flags
        NULL,          // use parent's environment
        NULL,          // use parent's current directory
        &siStartInfo,  // STARTUPINFO pointer
        &piProcInfo);  // receives PROCESS_INFORMATION

     if (bSuccess)
     {
         // Close handles to the stdin and stdout pipes no longer needed by the child process.
         // If they are not explicitly closed, there is no way to recognize that the child process has ended.
         CloseHandle( g_hChildStd_OUT_Wr );
         g_hChildStd_OUT_Wr = NULL;
         CloseHandle( g_hChildStd_IN_Rd );
         g_hChildStd_IN_Rd = NULL;

         try
         {
             bool wSuccess = true;

             // stdin if we're using no placeholder, old behaviour for backwards compatibility so it doesn't break somebody's script
             if (useStdinForFilepath) {
                 DWORD dwWritten;
                 wSuccess = WriteFile(g_hChildStd_IN_Wr, filepath_c, (DWORD)strlen( filepath_c ), &dwWritten, NULL );
             }

             CloseHandle( g_hChildStd_IN_Wr );
             g_hChildStd_IN_Wr = NULL;

             bool rSuccess = readFromPipe(g_hChildStd_OUT_Rd, artwork_url, timeout);

             if ( wSuccess && rSuccess )
             {
                 WaitForSingleObject( piProcInfo.hProcess, 5000 );
             }
         }
         catch (...)
         {
             // Make sure handles are close in case of error
             CloseHandle( piProcInfo.hProcess );
             CloseHandle( piProcInfo.hThread );
             throw;
         }

         DWORD exit_code;
         GetExitCodeProcess( piProcInfo.hProcess, &exit_code );
         bool terminateProcess = exit_code == STILL_ACTIVE && artwork_url.get_length() == 0;

         // In case of a time out terminate the process
         if (terminateProcess)
         {
             TerminateProcess(piProcInfo.hProcess, exit_code);
             WaitForSingleObject( piProcInfo.hProcess, 5000 );
         }

         CloseHandle( piProcInfo.hProcess );
         CloseHandle( piProcInfo.hThread );

         // If exit code is zero and result contains newlines assume it's an error since urls should not contains those
         // Also does simple validation on the URL
         const bool isError = exit_code != 0 || artwork_url.find_first('\n') != ~0 || !isValidUrl(artwork_url);
         artwork_url =  terminateProcess ? pfc::string8("Process timed out") : artwork_url;

         FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": " << uploaderLabel << " exited with status: " << exit_code <<
             " and " << ( isError ? "error" : "url" ) << ": " << artwork_url;

         if (isError)
         {
             artwork_url = "";
         }

         return true;
     }

    // What's the actual error? We care.
    DWORD lastError = GetLastError();
    std::string errorDesc = getWindowsErrorDescription(lastError);
    std::string errorName = getWindowsErrorName( lastError );

    closePipes( g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd, g_hChildStd_OUT_Wr );
    g_hChildStd_IN_Rd = NULL;
    g_hChildStd_IN_Wr = NULL;
    g_hChildStd_OUT_Rd = NULL;
    g_hChildStd_OUT_Wr = NULL;

    const std::string executableStr = wideToUtf8( executable );
    if ( !executableStr.empty() )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Failed to execute " << uploaderLabel
                                 << " '" << executableStr.c_str()
                                 << "' (Windows error: " << lastError
                                 << " - " << errorName.c_str()
                                 << " - " << errorDesc.c_str()
                                 << "; binary: " << getBinaryTypeDisplay( executable ).c_str()
                                 << "; file size: " << getFileSizeDisplay( executable ).c_str()
                                 << ")";
    }
    else
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Failed to execute " << uploaderLabel
                                 << " command (Windows error: " << lastError
                                 << " - " << errorName.c_str()
                                 << " - " << errorDesc.c_str()
                                 << ")";
    }

    return false;
}

bool usesUrlPlaceholder(const std::string &commandString)
{
    return commandContainsPlaceholder( commandString, "{url}" );
}

bool usesBlurPercentPlaceholder(const std::string &commandString)
{
    return commandContainsPlaceholder( commandString, "{blur_percent}" );
}

static void rememberUploadedArtworkHash( artwork_info& art,
                                         abort_callback& abort,
                                         const pfc::string8& artwork_url,
                                         const UploadOptions& options )
{
    if ( artwork_url.get_length() == 0 )
    {
        return;
    }

    ensureArtworkHash( art );
    if ( art.artwork_hash.get_length() > 0 )
    {
        set_artwork_url_hash( artwork_url,
                              art.artwork_hash,
                              abort,
                              options.mode,
                              options.mode == ArtworkMode::Blurred ? options.blurPercent : 0 );
    }
}

static std::chrono::seconds getCustomCommandTimeout()
{
    const long timeoutConfig = config::processTimeout.GetValue();
    return std::chrono::seconds( timeoutConfig == 0 ? 86400 : timeoutConfig );
}

static pfc::string8 uploadArtworkWithCustomCommand( artwork_info& art,
                                                    abort_callback &abort,
                                                    const UploadOptions& options,
                                                    metadb_index_hash hash )
{
    pfc::string8 artwork_url = "";
    const std::string commandString = config::uploadArtworkCommand.GetValue();
    const bool hasFilePathPlaceholder = usesFilePathPlaceholder(commandString);
    const bool hasUrlPlaceholder = usesUrlPlaceholder(commandString);
    const bool hasBlurPercentPlaceholder = usesBlurPercentPlaceholder(commandString);

    if ( !validateCustomCommandBlurredUploadSupport( options, commandString ) )
    {
        return artwork_url;
    }

    const auto cachedUrl = getModeScopedCachedUrl( art, abort, hash, options );
    const auto placeholderUrl = hasUrlPlaceholder ? getModeScopedPlaceholderUrl( art, abort, hash, options ) : pfc::string8{};

    if ( cachedUrl.get_length() > 0 )
    {
        if ( commandString.length() == 0 || !hasUrlPlaceholder || !options.allowUrlPlaceholderReuse )
        {
            #ifdef _DEBUG
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                     << ": Using cached URL directly without invoking the uploader: "
                                     << cachedUrl.c_str();
            #endif
            return cachedUrl;
        }

        #ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Found cached URL, will call external tool with mode-scoped {url}: "
                                 << placeholderUrl.c_str();
        #endif
    }

    if ( commandString.length() == 0 )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": No upload command given";
        return artwork_url;
    }

    // Validate command string
    std::string validationError;
    if (!validateCommandString(commandString, validationError)) {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Invalid upload command: " << validationError;
        return artwork_url;
    }

    const bool needsArtworkFile = hasFilePathPlaceholder || !hasUrlPlaceholder;
    pfc::string8 tempFile;
    bool deleteFile = false;
    pfc::string8 filepath;

    if (needsArtworkFile)
    {
        if (!getArtworkFilepath(art, abort, filepath, tempFile, deleteFile))
        {
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Could not resolve a usable artwork file for upload command";
            return artwork_url;
        }
    }
    else if (!hasArtworkSource(art))
    {
        if (placeholderUrl.get_length() == 0)
        {
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Cannot execute {url} upload command without a cached artwork URL";
            return artwork_url;
        }
    }

    abort.check();

    const auto filepath_c = filepath.c_str();
   
    HANDLE g_hChildStd_IN_Rd = NULL;
    HANDLE g_hChildStd_IN_Wr = NULL;
    HANDLE g_hChildStd_OUT_Rd = NULL;
    HANDLE g_hChildStd_OUT_Wr = NULL;
    
    SECURITY_ATTRIBUTES saAttr;

     try
     {
         if ( !initializeProcessVariables(g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd, g_hChildStd_OUT_Wr, saAttr) )
         {
             closePipes(g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd, g_hChildStd_OUT_Wr);
             return artwork_url;
         }

         STARTUPINFO siStartInfo;

         ZeroMemory( &siStartInfo, sizeof(STARTUPINFO) );
         siStartInfo.cb = sizeof(STARTUPINFO);
         siStartInfo.hStdError = g_hChildStd_OUT_Wr;
         siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
         siStartInfo.hStdInput = g_hChildStd_IN_Rd;
         siStartInfo.dwFlags |= STARTF_USESTDHANDLES;


         PROCESS_INFORMATION piProcInfo; 
         ZeroMemory( &piProcInfo, sizeof(PROCESS_INFORMATION) );

         #ifdef _DEBUG
         FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Upload command " << commandString;
         FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Cover file path " << filepath;
         if (hasUrlPlaceholder) {
             FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Cached URL being passed: " << placeholderUrl.c_str();
         }
         #endif

         // Parse the command to separate executable from full command line
         // Pass filepath and/or URL if placeholders are used
         const auto parsedCmd = parseCommand(commandString,
             hasFilePathPlaceholder ? filepath.c_str() : "",
             hasUrlPlaceholder ? placeholderUrl.c_str() : "",
             std::to_string( options.mode == ArtworkMode::Blurred ? options.blurPercent : 0 ) );
         const auto& executable = parsedCmd.first;
         const auto& fullCmdLine = parsedCmd.second;

         #ifdef _DEBUG
         qwr::u8string fullCmd_utf8;
         fullCmd_utf8 = qwr::unicode::ToU8(fullCmdLine);
         FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Final command after placeholder substitution: " << fullCmd_utf8;
         #endif

         abort.check();

         // If using any placeholder, skip writing to stdin, the user should not need that
         // If not using *any* placeholder, use stdin for backwards compatibility, assume user has older upload script
         uploadOpenProcess(executable, fullCmdLine, filepath_c, artwork_url,
             siStartInfo, piProcInfo,
             g_hChildStd_OUT_Wr, g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd,
             !(hasFilePathPlaceholder || hasUrlPlaceholder || hasBlurPercentPlaceholder),
             getCustomCommandTimeout(),
             "artwork uploader");

         #ifdef _DEBUG
         FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": External command returned: '" << (artwork_url.get_length() > 0 ? artwork_url.c_str() : "(empty)") << "'";
         #endif

         // If using {url} placeholder and external command returned empty, return cached URL directly
         if (hasUrlPlaceholder && options.allowUrlPlaceholderReuse && artwork_url.get_length() == 0) {
             #ifdef _DEBUG
             FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": External command returned empty, falling back to cached URL: " << placeholderUrl.c_str();
             #endif
             if ( deleteFile )
             {
                 filesystem::g_remove(tempFile, abort);
             }
             return placeholderUrl;
         }
     } catch (...)
     {
         closePipes(g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd, g_hChildStd_OUT_Wr);

         if ( deleteFile )
         {
             filesystem::g_remove( tempFile, abort );
         }
         
         FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": artwork uploader threw an error";
         throw;
     }

    if ( deleteFile )
    {
        filesystem::g_remove(tempFile, abort);
    }

    abort.check();

    if (artwork_url.get_length() > 0)
    {
        rememberUploadedArtworkHash( art, abort, artwork_url, options );
        #ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Using external command result: " << artwork_url.c_str();
        #endif
    }

    return artwork_url;
}

static std::wstring quoteWindowsArgumentWide( std::wstring_view value )
{
    std::wstring quoted;
    quoted.reserve( value.size() + 2 );
    quoted.push_back( L'"' );

    size_t backslashCount = 0;
    for ( const wchar_t ch: value )
    {
        if ( ch == L'\\' )
        {
            ++backslashCount;
            continue;
        }

        if ( ch == L'"' )
        {
            quoted.append( backslashCount * 2 + 1, L'\\' );
            quoted.push_back( ch );
            backslashCount = 0;
            continue;
        }

        quoted.append( backslashCount, L'\\' );
        backslashCount = 0;
        quoted.push_back( ch );
    }

    quoted.append( backslashCount * 2, L'\\' );
    quoted.push_back( L'"' );

    return quoted;
}

static std::wstring buildBundledUploaderCommandLine( const pfc::string8& executablePath,
                                                     const pfc::string8& artworkPath,
                                                     const UploadOptions& options )
{
    std::wstring commandLine = quoteWindowsArgumentWide( qwr::unicode::ToWide( executablePath ) );

    const auto serviceCliName = config::GetBundledUploaderServiceCliName();
    commandLine += L" --service ";
    commandLine += quoteWindowsArgumentWide( qwr::unicode::ToWide( qwr::u8string_view{ serviceCliName.data(), serviceCliName.size() } ) );

    const auto apiKey = config::TrimWhitespace( config::bundledUploaderApiKey.GetValue() );
    if ( !apiKey.empty() )
    {
        commandLine += L" --api-key ";
        commandLine += quoteWindowsArgumentWide( qwr::unicode::ToWide( qwr::u8string_view{ apiKey.data(), apiKey.size() } ) );
    }

    if ( options.mode == ArtworkMode::Blurred )
    {
        const auto blurPercent = std::to_string( options.blurPercent );
        commandLine += L" --blur-percent ";
        commandLine += quoteWindowsArgumentWide( qwr::unicode::ToWide( qwr::u8string_view{ blurPercent.data(), blurPercent.size() } ) );
    }

    commandLine += L" ";
    commandLine += quoteWindowsArgumentWide( qwr::unicode::ToWide( artworkPath ) );

    return commandLine;
}

static std::chrono::seconds getBundledUploaderProcessTimeout()
{
    return std::chrono::seconds( 120 );
}

static pfc::string8 uploadArtworkWithBundledUploader( artwork_info& art,
                                                      abort_callback& abort,
                                                      const UploadOptions& options,
                                                      metadb_index_hash hash )
{
    pfc::string8 artwork_url;

    if ( !validateBlurredUploadEnabled( options ) )
    {
        return artwork_url;
    }

    const auto cachedUrl = getModeScopedCachedUrl( art, abort, hash, options );
    if ( cachedUrl.get_length() > 0 )
    {
#ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Using cached URL directly without invoking the bundled uploader: "
                                 << cachedUrl.c_str();
#endif
        return cachedUrl;
    }

    pfc::string8 tempFile;
    bool deleteFile = false;
    pfc::string8 filepath;
    if ( !getArtworkFilepath( art, abort, filepath, tempFile, deleteFile ) )
    {
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Could not resolve a usable artwork file for bundled upload";
        return artwork_url;
    }

    auto cleanupTempFile = [&] {
        if ( deleteFile )
        {
            filesystem::g_remove( tempFile, abort );
        }
    };

    pfc::string8 executablePath;
    const BundledUploaderBinary* selectedBinary = nullptr;
    if ( !ensureBundledUploaderReady( abort, executablePath, selectedBinary ) )
    {
        cleanupTempFile();
        return artwork_url;
    }

    HANDLE g_hChildStd_IN_Rd = NULL;
    HANDLE g_hChildStd_IN_Wr = NULL;
    HANDLE g_hChildStd_OUT_Rd = NULL;
    HANDLE g_hChildStd_OUT_Wr = NULL;

    SECURITY_ATTRIBUTES saAttr;

    try
    {
        if ( !initializeProcessVariables( g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd, g_hChildStd_OUT_Wr, saAttr ) )
        {
            closePipes( g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd, g_hChildStd_OUT_Wr );
            cleanupTempFile();
            return artwork_url;
        }

        STARTUPINFO siStartInfo;
        ZeroMemory( &siStartInfo, sizeof( STARTUPINFO ) );
        siStartInfo.cb = sizeof( STARTUPINFO );
        siStartInfo.hStdError = g_hChildStd_OUT_Wr;
        siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
        siStartInfo.hStdInput = g_hChildStd_IN_Rd;
        siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

        PROCESS_INFORMATION piProcInfo;
        ZeroMemory( &piProcInfo, sizeof( PROCESS_INFORMATION ) );

        const auto fullCmdLine = buildBundledUploaderCommandLine( executablePath, filepath, options );
        const auto executable = qwr::unicode::ToWide( executablePath );

#ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION
                                 << ": Running bundled artwork uploader with service "
                                 << config::GetBundledUploaderServiceCliName().c_str()
                                 << " using " << selectedBinary->archName;
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Cover file path " << filepath;
#endif

        abort.check();

        pfc::string8 uploaderLabel = "bundled artwork uploader (";
        uploaderLabel += selectedBinary->archName;
        uploaderLabel += ")";

        uploadOpenProcess( executable,
                           fullCmdLine,
                           "",
                           artwork_url,
                           siStartInfo,
                           piProcInfo,
                           g_hChildStd_OUT_Wr,
                           g_hChildStd_IN_Rd,
                           g_hChildStd_IN_Wr,
                           g_hChildStd_OUT_Rd,
                           false,
                           getBundledUploaderProcessTimeout(),
                           uploaderLabel.c_str() );
    }
    catch ( ... )
    {
        closePipes( g_hChildStd_IN_Rd, g_hChildStd_IN_Wr, g_hChildStd_OUT_Rd, g_hChildStd_OUT_Wr );
        cleanupTempFile();

        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": bundled artwork uploader threw an error";
        throw;
    }

    cleanupTempFile();
    abort.check();

    if ( artwork_url.get_length() > 0 )
    {
        rememberUploadedArtworkHash( art, abort, artwork_url, options );
#ifdef _DEBUG
        FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": Using bundled uploader result: " << artwork_url.c_str();
#endif
    }

    return artwork_url;
}

pfc::string8 uploadArtwork( artwork_info& art,
                            abort_callback &abort,
                            const UploadOptions& options,
                            metadb_index_hash hash )
{
    if ( config::GetValidatedUploaderMode() == config::UploaderMode::Bundled )
    {
        return uploadArtworkWithBundledUploader( art, abort, options, hash );
    }

    return uploadArtworkWithCustomCommand( art, abort, options, hash );
}

}
