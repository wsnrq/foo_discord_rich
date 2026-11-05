#include <stdafx.h>

#include "discord_impl.h"

#include <fb2k/config.h>

#include <ctime>

#include "uploader.h"
#include <fb2k/artwork_metadb.h>

namespace drp::internal
{

PresenceData::PresenceData()
{
    memset( &presence, 0, sizeof( presence ) );
    presence.activityType = LISTENING;  // Default to listening for music
    presence.statusDisplayType = NAME;   // Default to showing app name
    presence.state = reinterpret_cast<const char*>( state.c_str() );
    presence.details = reinterpret_cast<const char*>( details.c_str() );
    presence.largeImageText = reinterpret_cast<const char*>( largeImageText.c_str() );
}

PresenceData::PresenceData( const PresenceData& other )
{
    CopyData( other );
}

PresenceData& PresenceData::operator=( const PresenceData& other )
{
    if ( this != &other )
    {
        CopyData( other );
    }

    return *this;
}

bool PresenceData::operator==( const PresenceData& other )
{
    auto areStringsSame = []( const char* a, const char* b ) {
        return ( ( a == b ) || ( a && b && !strcmp( a, b ) ) );
    };

    return ( areStringsSame( presence.state, other.presence.state )
             && areStringsSame( presence.details, other.presence.details )
             && areStringsSame( presence.largeImageKey, other.presence.largeImageKey )
             && areStringsSame( presence.largeImageText, other.presence.largeImageText )
             && areStringsSame( presence.smallImageKey, other.presence.smallImageKey )
             && areStringsSame( presence.smallImageText, other.presence.smallImageText )
             && metadb == other.metadb
             && remoteCoverUrl == other.remoteCoverUrl
             && presence.activityType == other.presence.activityType
             && presence.statusDisplayType == other.presence.statusDisplayType
             && presence.startTimestamp == other.presence.startTimestamp
             && presence.endTimestamp == other.presence.endTimestamp
             && imageRevision == other.imageRevision
             && trackLength == other.trackLength );
}

bool PresenceData::operator!=( const PresenceData& other )
{
    return !operator==( other );
}

void PresenceData::CopyData( const PresenceData& other )
{
    metadb = other.metadb;
    state = other.state;
    details = other.details;
    largeImageText = other.largeImageText;
    largeImageKey = other.largeImageKey;
    smallImageKey = other.smallImageKey;
    remoteCoverUrl = other.remoteCoverUrl;
    imageRevision = other.imageRevision;
    trackLength = other.trackLength;

    memcpy( &presence, &other.presence, sizeof( presence ) );
    presence.state = reinterpret_cast<const char*>( state.c_str() );
    presence.details = reinterpret_cast<const char*>( details.c_str() );
    presence.largeImageText = reinterpret_cast<const char*>( largeImageText.c_str() );
    presence.largeImageKey = ( largeImageKey.empty() ? nullptr : reinterpret_cast<const char*>( largeImageKey.c_str() ) );
    presence.smallImageKey = ( smallImageKey.empty() ? nullptr : reinterpret_cast<const char*>( smallImageKey.c_str() ) );
}

} // namespace drp::internal

namespace drp
{

PresenceModifier::PresenceModifier( DiscordHandler& parent,
                                    const std::shared_ptr<drp::internal::PresenceData> presenceData )
    : parent_( parent )
    , presenceData_( presenceData )
{
}

PresenceModifier::~PresenceModifier()
{
    const bool hasChanged = ( *parent_.presenceData_ != *presenceData_ );
    if ( hasChanged )
    {
        parent_.presenceData_ = presenceData_;
    }

    const bool needsToBeDisabled = ( isDisabled_
                                     || !playback_control::get()->is_playing()
                                     || ( playback_control::get()->is_paused() && config::disableWhenPaused ) );
    if ( needsToBeDisabled )
    {
        if ( parent_.HasPresence() )
        {
            parent_.ClearPresence();
        }
    }
    else
    {
        if ( !parent_.HasPresence() || hasChanged )
        {
            parent_.SendPresence();
        }
    }
}

static void setImageKey(const qwr::u8string& imageKey, std::shared_ptr<internal::PresenceData> pd)
{
    pd->largeImageKey = imageKey;
    pd->presence.largeImageKey = pd->largeImageKey.empty() ? nullptr : reinterpret_cast<const char*>( pd->largeImageKey.c_str() );
}

struct sharedData_t
{
    // Must be a shared_ptr or it will be unloaded from memory during the threaded operation
    std::shared_ptr<internal::PresenceData> pm;
    // Normal pointers should be fine for this as it's a static instance
    DiscordHandler* handler;
};

namespace
{

void setStaticLargeImage( std::shared_ptr<internal::PresenceData> pd )
{
    switch ( config::largeImageSettings )
    {
    case config::ImageSetting::Light:
        setImageKey( config::largeImageId_Light, pd );
        break;
    case config::ImageSetting::Dark:
        setImageKey( config::largeImageId_Dark, pd );
        break;
    case config::ImageSetting::Disabled:
        setImageKey( qwr::u8string{}, pd );
        break;
    }
}

void setHiddenFallbackImage( std::shared_ptr<internal::PresenceData> pd )
{
    const auto placeholderUrl = config::GetValidatedHiddenPlaceholderUrl();
    if ( !placeholderUrl.empty() )
    {
        setImageKey( qwr::u8string{ placeholderUrl }, pd );
        return;
    }

    setStaticLargeImage( pd );
}

uploader::UploadOptions makeRuntimeUploadOptions( ArtworkMode mode )
{
    uploader::UploadOptions options;
    options.mode = mode;
    options.allowCachedReuse = true;
    options.allowUrlPlaceholderReuse = ( mode == ArtworkMode::Normal );
    if ( mode == ArtworkMode::Blurred )
    {
        options.blurPercent = config::GetValidatedBlurPercent();
        options.allowUrlPlaceholderReuse = false;
    }

    return options;
}

bool ShouldAutomaticallyUploadArtwork()
{
    return config::autoUploadArtwork
           && config::uploadArtwork;
}

} // namespace

void PresenceModifier::UpdateImage()
{
    auto pc = playback_control::get();
    auto pd = presenceData_;
    pd->imageRevision = parent_.NextImageRevision();
    
    if (config::largeImageSettings == config::ImageSetting::Disabled)
    {
        setImageKey( qwr::u8string{}, pd );
        return;
    }

    const auto artworkOverrideUrl = config::GetValidatedArtworkOverrideUrl( pd->metadb );
    if ( !artworkOverrideUrl.empty() )
    {
        setImageKey( qwr::u8string{ artworkOverrideUrl }, pd );
        return;
    }

    if ( !config::uploadArtwork )
    {
        setStaticLargeImage( pd );
        return;
    }

    metadb_handle_ptr p_out;
    const bool gSuccess = pc->get_now_playing( p_out );
    metadb_index_hash hash;
    record_t rec;
    auto artworkMode = ArtworkMode::Normal;

    if ( gSuccess )
    {
        clientByGUID( guid::artwork_url_index )->hashHandle( p_out, hash );
        rec = record_get( hash );
        artworkMode = rec.artwork_mode;
    }

    if ( artworkMode == ArtworkMode::Hidden )
    {
        setHiddenFallbackImage( pd );
        return;
    }

    if ( artworkMode == ArtworkMode::Normal )
    {
        if ( uploader::isValidUrl( pd->remoteCoverUrl ) )
        {
            setImageKey( pd->remoteCoverUrl.toString(), pd );
            return;
        }

        const bool hasCanonicalUrl = uploader::isValidUrl( rec.artwork_url );
        if ( hasCanonicalUrl )
        {
            setImageKey( rec.artwork_url.toString(), pd );
            if ( !ShouldAutomaticallyUploadArtwork()
                 || config::GetValidatedUploaderMode() != config::UploaderMode::CustomCommand
                 || !uploader::usesUrlPlaceholder( config::uploadArtworkCommand.GetValue() ) )
            {
                return;
            }
        }
        else
        {
            setStaticLargeImage( pd );
        }

        if ( !gSuccess )
        {
            return;
        }

        if ( !ShouldAutomaticallyUploadArtwork() )
        {
            return;
        }
    }
    else
    {
        if ( !ShouldAutomaticallyUploadArtwork() )
        {
            const auto blurredArtworkUrl = GetEligibleBlurredArtworkUrl( rec );
            if ( uploader::isValidUrl( blurredArtworkUrl ) )
            {
                setImageKey( blurredArtworkUrl.toString(), pd );
            }
            else
            {
                setHiddenFallbackImage( pd );
            }

            return;
        }

        setHiddenFallbackImage( pd );
    }

    const auto options = makeRuntimeUploadOptions( artworkMode );
    if ( artworkMode == ArtworkMode::Blurred && options.blurPercent == 0 )
    {
        return;
    }

    auto shared = std::make_shared<sharedData_t>();
    shared->pm = pd;
    shared->handler = &parent_;

    fb2k::splitTask( [p_out, hash, shared, options]{
        // In worker thread!
        try {
            pfc::string8 artwork_url;
            bool recordChanged = false;
            if ( uploader::extractAndUploadArtwork( p_out, fb2k::noAbort, artwork_url, hash, options, &recordChanged ) )
            {
                if ( recordChanged )
                {
                    fb2k::inMainThread( [hash] {
                        pfc::list_t<metadb_index_hash> changed;
                        changed += hash;
                        cached_index_api()->dispatch_refresh( guid::artwork_url_index, changed );
                    } );
                }

                setImageKey( artwork_url.toString(), shared->pm );
                shared->handler->MaybeUpdatePresence( shared->pm );
            }
        } catch(std::exception const & e) {
            // should not really get here
            FB2K_console_formatter() << DRP_NAME_WITH_VERSION << "Critical error: " << e;
        }
    } );
}

void PresenceModifier::UpdateImageForCoverUrl( const pfc::string8& url )
{
    presenceData_->remoteCoverUrl = url;
    UpdateImage();
}

void PresenceModifier::ClearCoverUrlAndUpdateImage()
{
    presenceData_->remoteCoverUrl = "";
    UpdateImage();
}

void PresenceModifier::UpdateSmallImage()
{
    auto pd = presenceData_;
    auto pc = playback_control::get();

    auto setImageKey = [&pd]( const qwr::u8string& imageKey ) {
        pd->smallImageKey = imageKey;
        pd->presence.smallImageKey = pd->smallImageKey.empty() ? nullptr : pd->smallImageKey.c_str();
    };

    const bool usePausedImage = ( pc->is_paused() || config::swapSmallImages );

    switch ( config::smallImageSettings )
    {
    case config::ImageSetting::Light:
    {
        setImageKey( usePausedImage ? config::pausedImageId_Light : config::playingImageId_Light );
        break;
    }
    case config::ImageSetting::Dark:
    {
        setImageKey( usePausedImage ? config::pausedImageId_Dark : config::playingImageId_Dark );
        break;
    }
    case config::ImageSetting::Disabled:
    {
        setImageKey( qwr::u8string{} );
        break;
    }
    }
}

/**
 * https://stackoverflow.com/a/59691895
 * Calculate the number of characters in a utf-8 string.
 *  Some composite characters that are composed of multiple different characters, such as some emojis,
 *  are counted as multiple characters.
 */
size_t count_codepoints( const qwr::u8string& str )
{
    size_t count = 0;
    for ( auto& c: str )
        if ( ( c & 0b1100'0000 ) != 0b1000'0000 ) // Not a trailing byte
            ++count;
    return count;
}

void PresenceModifier::UpdateTrack( metadb_handle_ptr metadb )
{
    auto pd = presenceData_;

    pd->state.clear();
    pd->details.clear();
    pd->largeImageText.clear();
    pd->trackLength = 0;

    if ( metadb.is_valid() )
    { // Need to save, since refresh might be required when settings are changed
        pd->metadb = metadb;
    }

    auto pc = playback_control::get();
    const auto queryData = [&pc, metadb = pd->metadb]( const qwr::u8string& query ) -> qwr::u8string {
        titleformat_object::ptr tf;
        titleformat_compiler::get()->compile_safe( tf, query.c_str() );
        pfc::string8_fast result;

        if ( pc->is_playing() )
        {
            metadb_handle_ptr dummyHandle;
            pc->playback_format_title_ex( dummyHandle, nullptr, result, tf, nullptr, playback_control::display_level_all );
        }
        else if ( metadb.is_valid() )
        {
            metadb->format_title( nullptr, result, tf, nullptr );
        }

        return result.c_str();
    };
    const auto fixStringLength = []( qwr::u8string& str ) {
        // Required for correct calculation of utf-8 string length
        if ( count_codepoints(str) == 1 )
        { // minimum allowed non-zero string length is 2, so we need to pad it
            str += ' ';
        }
        // Normal length used here as resizing truncates the string to 127 bytes anyways
        else if ( str.length() > 127 )
        { // maximum allowed length is 127
            str.resize( 127 );
        }
    };

    pd->state = queryData( config::stateQuery );
    fixStringLength( pd->state );
    pd->details = queryData( config::detailsQuery );
    fixStringLength( pd->details );
    pd->largeImageText = queryData( config::largeImageTextQuery );
    fixStringLength( pd->largeImageText );

    // update status display type
    switch ( config::statusSettings )
    {
    case config::StatusSetting::Name:
    {
        pd->presence.statusDisplayType = NAME;
        break;
    }
    case config::StatusSetting::Middle:
    {
        pd->presence.statusDisplayType = STATE;
        break;
    }
    case config::StatusSetting::Top:
    {
        pd->presence.statusDisplayType = DETAILS;
        break;
    }
    }

    const qwr::u8string lengthStr = queryData( "[%length_seconds_fp%]" );
    pd->trackLength = ( lengthStr.empty() ? 0 : stold( lengthStr ) );

    const qwr::u8string durationStr = queryData( "[%playback_time_seconds%]" );

    pd->presence.state = pd->state.c_str();
    pd->presence.details = pd->details.c_str();
    pd->presence.largeImageText = pd->largeImageText.c_str();
    UpdateDuration( durationStr.empty() ? 0 : stold( durationStr ) );
}

void PresenceModifier::UpdateDuration( double time )
{
    auto pd = presenceData_;
    auto pc = playback_control::get();
    const config::TimeSetting timeSetting = ( ( pd->trackLength && pc->is_playing() && !pc->is_paused() )
                                                  ? config::timeSettings
                                                  : config::TimeSetting::Disabled );
    switch ( timeSetting )
    {
    case config::TimeSetting::ProgressBar:
    {
        pd->presence.startTimestamp = std::time( nullptr ) - std::llround( time );
        pd->presence.endTimestamp = std::time( nullptr ) + std::max<uint64_t>( 0, std::llround( pd->trackLength - time ) );

        break;
    }
    case config::TimeSetting::Elapsed:
    {
        pd->presence.startTimestamp = std::time( nullptr ) - std::llround( time );
        pd->presence.endTimestamp = 0;

        break;
    }
    case config::TimeSetting::Disabled:
    {
        pd->presence.startTimestamp = 0;
        pd->presence.endTimestamp = 0;

        break;
    }
    }
}

void PresenceModifier::DisableDuration()
{
    auto pd = presenceData_;
    pd->presence.startTimestamp = 0;
    pd->presence.endTimestamp = 0;
}

void PresenceModifier::Disable()
{
    isDisabled_ = true;
}

DiscordHandler& DiscordHandler::GetInstance()
{
    static DiscordHandler discordHandler;
    return discordHandler;
}

uint64_t DiscordHandler::NextImageRevision()
{
    return nextImageRevision_++;
}

void DiscordHandler::Initialize()
{
    appToken_ = config::discordAppToken;

    // fix from RemuSalminen@d627c6e - ensure timeSettings has valid value
    if ( config::timeSettings != config::TimeSetting::ProgressBar && config::timeSettings != config::TimeSetting::Elapsed && config::timeSettings != config::TimeSetting::Disabled )
        config::timeSettings = config::TimeSetting::ProgressBar;

    DiscordEventHandlers handlers{};

    handlers.ready = OnReady;
    handlers.disconnected = OnDisconnected;
    handlers.errored = OnErrored;

    Discord_Initialize( appToken_.c_str(), &handlers, 1, nullptr );
    Discord_RunCallbacks();

    hasPresence_ = true; ///< Discord may use default app handler, which we need to override

    auto pm = GetPresenceModifier();
    pm.UpdateImage();
    pm.Disable(); ///< we don't want to activate presence yet
}

void DiscordHandler::Finalize()
{
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordHandler::OnSettingsChanged()
{
    if ( appToken_ != static_cast<std::string>( config::discordAppToken ) )
    {
        Finalize();
        Initialize();
    }

    auto pm = GetPresenceModifier();
    pm.UpdateImage();
    pm.UpdateSmallImage();
    pm.UpdateTrack();
    if ( !config::isEnabled )
    {
        pm.Disable();
    }
}

void DiscordHandler::MaybeUpdatePresence(std::shared_ptr<internal::PresenceData> pd)
{
    // If it's the same pointer it means the presence has not changed while uploading the cover
    // and we can continue to updating the presence with the cover url
    if (pd != presenceData_)
    {
        return;
    }

    if ( HasPresence() )
    {
        SendPresence();
    }
}

bool DiscordHandler::HasPresence() const
{
    return hasPresence_;
}

void DiscordHandler::SendPresence()
{
    if ( config::isEnabled )
    {
        Discord_UpdatePresence( &presenceData_->presence );
        hasPresence_ = true;
    }
    else
    {
        Discord_ClearPresence();
        hasPresence_ = false;
    }
    Discord_RunCallbacks();
}

void DiscordHandler::ClearPresence()
{
    Discord_ClearPresence();
    hasPresence_ = false;

    Discord_RunCallbacks();
}

PresenceModifier DiscordHandler::GetPresenceModifier()
{
    return PresenceModifier( *this, std::make_shared<internal::PresenceData>(*presenceData_) );
}

void DiscordHandler::OnReady( const DiscordUser* request )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": connected to " << ( request->username ? request->username : "<null>" );
}

void DiscordHandler::OnDisconnected( int errorCode, const char* message )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": disconnected with code " << errorCode;
    if ( message )
    {
        FB2K_console_formatter() << message;
    }
}

void DiscordHandler::OnErrored( int errorCode, const char* message )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": error " << errorCode;
    if ( message )
    {
        FB2K_console_formatter() << message;
    }
}

} // namespace drp
