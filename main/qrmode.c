#include "qrmode.h"

#include "bcur.h"
#include "button_events.h"
#include "gui.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "keychain.h"
#include "qrcode.h"
#include "qrscan.h"
#include "sensitive.h"
#include "storage.h"
#include "ui.h"
#include "utils/address.h"
#include "wallet.h"

#include <wally_script.h>

#include <string.h>

void make_show_qr_help_activity(gui_activity_t** activity_ptr, const char* url, Icon* qr_icon);

void make_show_xpub_qr_activity(gui_activity_t** activity_ptr, const char* label, const char* pathstr, Icon* icons,
    const size_t num_icons, const size_t frames_per_qr_icon);
void make_xpub_qr_options_activity(gui_activity_t** activity_ptr, gui_view_node_t** script_textbox,
    gui_view_node_t** multisig_textbox, gui_view_node_t** urtype_textbox);

void make_search_verify_address_activity(
    gui_activity_t** activity_ptr, const char* pathstr, progress_bar_t* progress_bar, gui_view_node_t** index_text);

void make_show_qr_activity(gui_activity_t** activity_ptr, const char* title, const char* label, Icon* icons,
    const size_t num_icons, const size_t frames_per_qr_icon);
void make_qr_options_activity(
    gui_activity_t** activity_ptr, gui_view_node_t** density_textbox, gui_view_node_t** speed_textbox);

// PSBT struct and functions
struct wally_psbt;
int sign_psbt(struct wally_psbt* psbt, const char** errmsg);
int wally_psbt_free(struct wally_psbt* psbt);

#define EXPORT_XPUB_PATH_LEN 4

static const uint8_t ADDRESS_SEARCH_BATCH_SIZE = 20;
static const uint16_t ADDRESS_NUM_INDEXES_TO_CHECK = 25 * ADDRESS_SEARCH_BATCH_SIZE;

// Test whether 'flags' contains the entirety of the 'test_flags'
// (ie. maybe compound/multiple bits set)
static inline bool contains_flags(const uint16_t flags, const uint16_t test_flags)
{
    return (flags & test_flags) == test_flags;
}

// Rotate through: low -> high -> high|low -> low -> high ...
// 'unset' treated as 'high' (ie. the middle value)
static void rotate_flags(uint16_t* flags, const uint16_t high, const uint16_t low)
{
    JADE_ASSERT(flags);

    if (contains_flags(*flags, high | low)) {
        *flags &= ~high;
    } else if (contains_flags(*flags, high)) {
        *flags |= low;
    } else if (contains_flags(*flags, low)) {
        *flags ^= (high | low);
    } else { // ie. currently 0/default/uninitialised - treat as 'high'
        *flags |= (high | low);
    }
}

static uint8_t qr_animation_speed_from_flags(const uint16_t qr_flags)
{
    // Frame periods around 800ms, 450ms, 270ms  (see GUI_TARGET_FRAMERATE)
    // Frame rates: HIGH|LOW > HIGH > LOW ...
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_SPEED_HIGH | QR_SPEED_LOW) ? 4 : contains_flags(qr_flags, QR_SPEED_LOW) ? 12 : 7;
}
static const char* qr_animation_speed_desc_from_flags(const uint16_t qr_flags)
{
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_SPEED_HIGH | QR_SPEED_LOW) ? "High"
        : contains_flags(qr_flags, QR_SPEED_LOW)                  ? "Low"
                                                                  : "Medium";
}

static uint8_t qr_version_from_flags(const uint16_t qr_flags)
{
    // QR versions 12, 6 and 4 fit well on the Jade screen with scaling of
    // 2 px-per-cell, 3 px-per-cell, and 4 px-per-cell respectively.
    // Version/Size/Density: HIGH|LOW > HIGH > LOW ... 0 implies unset/default
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_DENSITY_HIGH | QR_DENSITY_LOW) ? 12
        : contains_flags(qr_flags, QR_DENSITY_LOW)                    ? 4
                                                                      : 6;
}
static const char* qr_density_desc_from_flags(const uint16_t qr_flags)
{
    // unset/default is treated as 'high' (ie. the middle value)
    return contains_flags(qr_flags, QR_DENSITY_HIGH | QR_DENSITY_LOW) ? "High"
        : contains_flags(qr_flags, QR_DENSITY_LOW)                    ? "Low"
                                                                      : "Medium";
}

// We support native segwit and p2sh-wrapped segwit, siglesig and multisig
static script_variant_t xpub_script_variant_from_flags(const uint16_t qr_flags)
{
    const bool wrapped_segwit = contains_flags(qr_flags, QR_XPUB_P2SH_WRAPPED);
    if (contains_flags(qr_flags, QR_XPUB_MULTISIG)) {
        return wrapped_segwit ? MULTI_P2WSH_P2SH : MULTI_P2WSH;
    }
    return wrapped_segwit ? MULTI_P2WSH_P2SH : MULTI_P2WSH;
}

static void create_display_xpub_qr_activity(gui_activity_t** activity_ptr, const uint16_t qr_flags)
{
    JADE_ASSERT(activity_ptr);

    const bool use_format_hdkey = false; // qr_flags & QR_XPUB_HDKEY;  - not currently in use
    const char* const xpub_qr_format = use_format_hdkey ? BCUR_TYPE_CRYPTO_HDKEY : BCUR_TYPE_CRYPTO_ACCOUNT;
    const script_variant_t script_variant = xpub_script_variant_from_flags(qr_flags);

    // Deduce path based on script type and main/test network restrictions
    uint32_t path[EXPORT_XPUB_PATH_LEN]; // 3 or 4 - purpose'/cointype'/account'/[multisig bip48 script type']
    size_t path_len = 0;
    wallet_get_default_xpub_export_path(script_variant, path, EXPORT_XPUB_PATH_LEN, &path_len);

    // Construct BC-UR CBOR message for 'crypto-account' or 'crypto-hdkey' bcur
    uint8_t cbor[128];
    size_t written = 0;
    if (use_format_hdkey) {
        bcur_build_cbor_crypto_hdkey(path, path_len, cbor, sizeof(cbor), &written);
    } else {
        bcur_build_cbor_crypto_account(script_variant, path, path_len, cbor, sizeof(cbor), &written);
    }

    // Map BCUR cbor into a series of QR-code icons
    Icon* icons = NULL;
    size_t num_icons = 0;
    const uint8_t qrcode_version = qr_version_from_flags(qr_flags);
    bcur_create_qr_icons(cbor, written, xpub_qr_format, qrcode_version, &icons, &num_icons);

    // Create xpub activity for those icons
    char pathstr[32];
    const bool ret = bip32_path_as_str(path, path_len, pathstr, sizeof(pathstr));
    JADE_ASSERT(ret);
    const char* label = qr_flags & QR_XPUB_MULTISIG ? "Multisig" : "Singlesig";
    const uint8_t frames_per_qr = qr_animation_speed_from_flags(qr_flags);
    make_show_xpub_qr_activity(activity_ptr, label, pathstr, icons, num_icons, frames_per_qr);
    JADE_ASSERT(*activity_ptr);
}

static bool handle_xpub_options(uint16_t* qr_flags)
{
    JADE_ASSERT(qr_flags);

    gui_activity_t* activity = NULL;
    gui_view_node_t* script_textbox = NULL;
    gui_view_node_t* multisig_textbox = NULL;
    gui_view_node_t* urtype_textbox = NULL;
    make_xpub_qr_options_activity(&activity, &script_textbox, &multisig_textbox, &urtype_textbox);
    JADE_ASSERT(activity);
    JADE_ASSERT(script_textbox);
    JADE_ASSERT(multisig_textbox);
    JADE_ASSERT(!urtype_textbox); // not currently in use

    const uint16_t initial_flags = *qr_flags;
    while (true) {
        // Update options
        gui_update_text(script_textbox, *qr_flags & QR_XPUB_P2SH_WRAPPED ? "Wrapped Segwit" : "Native Segwit");
        gui_update_text(multisig_textbox, *qr_flags & QR_XPUB_MULTISIG ? "Multisig" : "Singlesig");
        // gui_update_text(urtype_textbox, *qr_flags & QR_XPUB_HDKEY ? "HDKey" : "Account");  // not currently in use

        // Show, and await button click
        gui_set_current_activity(activity);

        int32_t ev_id;
#ifndef CONFIG_DEBUG_UNATTENDED_CI
        const bool ret = gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
#else
        gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, NULL, NULL,
            CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
        const bool ret = true;
        ev_id = BTN_XPUB_EXIT;
#endif
        if (ret) {
            if (ev_id == BTN_XPUB_TOGGLE_SCRIPT) {
                *qr_flags ^= QR_XPUB_P2SH_WRAPPED;
            } else if (ev_id == BTN_XPUB_TOGGLE_MULTISIG) {
                *qr_flags ^= QR_XPUB_MULTISIG;
                //} else if (ev_id == BTN_XPUB_TOGGLE_BCUR_TYPE) {
                //    *qr_flags ^= QR_XPUB_HDKEY;
            } else if (ev_id == BTN_XPUB_OPTIONS_HELP) {
                display_qr_help_screen("blockstream.com/xpub");
            } else if (ev_id == BTN_XPUB_OPTIONS_EXIT) {
                // Done
                break;
            }
        }
    }

    // If nothing was updated, return false
    if (initial_flags == *qr_flags) {
        return false;
    }

    // Persist prefereces and return true to indicate they were changed
    storage_set_qr_flags(*qr_flags);
    return true;
}

// Display singlesig xpub qr code
void display_xpub_qr(void)
{
    uint16_t qr_flags = storage_get_qr_flags();

    // Create show xpub activity for those icons
    gui_activity_t* activity = NULL;
    create_display_xpub_qr_activity(&activity, qr_flags);
    JADE_ASSERT(activity);

    while (true) {
        // Show, and await button click
        gui_set_current_activity(activity);

        int32_t ev_id;
#ifndef CONFIG_DEBUG_UNATTENDED_CI
        const bool ret = gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
#else
        gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, NULL, NULL,
            CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
        const bool ret = true;
        ev_id = BTN_XPUB_EXIT;
#endif
        if (ret) {
            if (ev_id == BTN_XPUB_OPTIONS) {
                if (handle_xpub_options(&qr_flags)) {
                    // Options were updated - re-create xpub screen
                    create_display_xpub_qr_activity(&activity, qr_flags);
                }
            } else if (ev_id == BTN_XPUB_EXIT) {
                // Done
                break;
            }
        }
    }
}

// Verify an address string by brute-forcing
static bool verify_address(const char* address)
{
    JADE_ASSERT(address);

    address_data_t addr_data;
    if (!parse_address(address, &addr_data)) {
        await_error_activity("Failed to parse address");
        return false;
    }

    char buf[160];
    int rc = snprintf(buf, sizeof(buf), "Attempt to verify address?\n\n%s", addr_data.address);
    JADE_ASSERT(rc > 0 && rc < sizeof(buf));
    if (!await_yesno_activity("Verify Address", buf, true)) {
        return false;
    }

    // check network - eg. testnet address, but this jade is setup for mainnet only
    if (!keychain_is_network_type_consistent(addr_data.network)) {
        await_error_activity("Network type inconsistent");
        return false;
    }

    // Map the script type
    size_t script_type = 0;
    if (wally_scriptpubkey_get_type(addr_data.script, addr_data.script_len, &script_type) != WALLY_OK) {
        await_error_activity("Failed to parse scriptpubkey");
        return false;
    }
    script_variant_t variant;
    if (!get_singlesig_variant_from_script_type(script_type, &variant)) {
        await_error_activity("Address scriptpubkey unsupported");
        return false;
    }

    // Get the path to search
    uint32_t path[EXPORT_XPUB_PATH_LEN];
    size_t path_len = 0;
    wallet_get_default_xpub_export_path(variant, path, EXPORT_XPUB_PATH_LEN, &path_len);
    JADE_ASSERT(path_len == EXPORT_XPUB_PATH_LEN - 1);
    path[path_len++] = 0; // 'external' (ie. not internal change) address

    // Get as hdkey
    bool verified = false;
    struct ext_key search_root;
    bool ret = wallet_get_hdkey(path, path_len, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &search_root);
    JADE_ASSERT(ret);

    // Search for the path that finds the given script
    char root_path[32];
    ret = bip32_path_as_str(path, path_len, root_path, sizeof(root_path));
    JADE_ASSERT(ret);
    gui_activity_t* activity = NULL;
    gui_view_node_t* index_text = NULL;
    progress_bar_t progress_bar = {};
    make_search_verify_address_activity(&activity, root_path, &progress_bar, &index_text);
    JADE_ASSERT(activity);
    JADE_ASSERT(index_text);

    // Make an event-data structure to track events - attached to the activity
    wait_event_data_t* const event_data = gui_activity_make_wait_event_data(activity);
    JADE_ASSERT(event_data);

    // ... and register against the activity - we will await btn events later
    gui_activity_register_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, sync_wait_event_handler, event_data);

    size_t index = 0;
    size_t confirmed_at_index = index;
    while (true) {
        gui_set_current_activity(activity);

        // Update the progress bar and text label
        char idx_txt[12];
        rc = snprintf(idx_txt, sizeof(idx_txt), "%u", index);
        JADE_ASSERT(rc > 0 && rc < sizeof(idx_txt));
        update_progress_bar(&progress_bar, ADDRESS_NUM_INDEXES_TO_CHECK, index - confirmed_at_index);
        gui_update_text(index_text, idx_txt);

        // Search a small batch of paths for the address script
        // NOTE: 'index' is updated as we go along
        if (wallet_search_for_singlesig_script(
                variant, &search_root, &index, ADDRESS_SEARCH_BATCH_SIZE, addr_data.script, addr_data.script_len)) {
            // Address script found and matched - verified
            // NOTE: 'index' will hold the relevant value
            verified = true;
            break;
        }

        // Every so often suggest to user that they might want to abandon the search
        if (index >= confirmed_at_index + ADDRESS_NUM_INDEXES_TO_CHECK) {
            rc = snprintf(buf, sizeof(buf), "Failed to verify address.\n\nCheck next %u addresses?",
                ADDRESS_NUM_INDEXES_TO_CHECK);
            JADE_ASSERT(rc > 0 && rc < sizeof(buf));
            if (!await_yesno_activity("Verify Address", buf, true)) {
                // Abandon - exit loop
                break;
            }
            confirmed_at_index = index;
        } else {
            // Giver user a chance to exit or to skip this batch of addresses
            int32_t ev_id = 0;
#ifndef CONFIG_DEBUG_UNATTENDED_CI
            ret = sync_wait_event(
                      GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, event_data, NULL, &ev_id, NULL, 10 / portTICK_PERIOD_MS)
                == ESP_OK;
#else
            sync_wait_event(GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, event_data, NULL, NULL, NULL,
                CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
            ret = index > 4 * ADDRESS_NUM_INDEXES_TO_CHECK; // let it run for a few batches, then exit
            ev_id = BTN_SCAN_ADDRESS_EXIT;
#endif
            if (ret) {
                if (ev_id == BTN_SCAN_ADDRESS_SKIP_ADDRESSES) {
                    // Jump to end of this batch
                    index = confirmed_at_index + ADDRESS_NUM_INDEXES_TO_CHECK;
                    confirmed_at_index = index;
                } else if (ev_id == BTN_SCAN_ADDRESS_EXIT) {
                    // Abandon - exit loop
                    break;
                }
            }
        }
    }

    if (verified) {
        rc = snprintf(buf, sizeof(buf), "Address verified at path:\n\n%s/%u", root_path, index);
        JADE_ASSERT(rc > 0 && rc < sizeof(buf));
        await_message_activity(buf);
    } else {
        await_error_activity("Address NOT verified!");
    }

    return verified;
}

// Handle QR Options dialog - ie. QR size and frame-rate
static bool handle_qr_options(uint16_t* qr_flags)
{
    JADE_ASSERT(qr_flags);

    gui_activity_t* activity = NULL;
    gui_view_node_t* density_textbox = NULL;
    gui_view_node_t* speed_textbox = NULL;
    make_qr_options_activity(&activity, &density_textbox, &speed_textbox);
    JADE_ASSERT(activity);
    JADE_ASSERT(density_textbox);
    JADE_ASSERT(speed_textbox);

    const uint16_t initial_flags = *qr_flags;
    while (true) {
        // Update options
        gui_update_text(density_textbox, qr_density_desc_from_flags(*qr_flags));
        gui_update_text(speed_textbox, qr_animation_speed_desc_from_flags(*qr_flags));

        // Show, and await button click
        gui_set_current_activity(activity);

        int32_t ev_id;
#ifndef CONFIG_DEBUG_UNATTENDED_CI
        const bool ret = gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
#else
        gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, NULL, NULL,
            CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
        const bool ret = true;
        ev_id = BTN_XPUB_EXIT;
#endif
        if (ret) {
            // NOTE: For Density and Speed :- HIGH|LOW > HIGH > LOW
            // Rotate through: LOW -> HIGH -> HIGH|LOW -> LOW -> ...
            // unset/default is treated as HIGH ie. the middle value
            if (ev_id == BTN_QR_TOGGLE_DENSITY) {
                rotate_flags(qr_flags, QR_DENSITY_HIGH, QR_DENSITY_LOW);
            } else if (ev_id == BTN_QR_TOGGLE_SPEED) {
                rotate_flags(qr_flags, QR_SPEED_HIGH, QR_SPEED_LOW);
            } else if (ev_id == BTN_QR_OPTIONS_HELP) {
                display_qr_help_screen("blockstream.com/scan");
            } else if (ev_id == BTN_QR_OPTIONS_EXIT) {
                // Done
                break;
            }
        }
    }

    // If nothing was updated, return false
    if (initial_flags == *qr_flags) {
        return false;
    }

    // Persist prefereces and return true to indicate they were changed
    storage_set_qr_flags(*qr_flags);
    return true;
}

// Create activity to display (potentially multi-frame/animated) qr
static void create_display_bcur_qr_activity(gui_activity_t** activity_ptr, const char* title, const char* label,
    const char* bcur_type, const uint8_t* cbor, const size_t cbor_len, const uint16_t qr_flags)
{
    JADE_ASSERT(activity_ptr);
    JADE_ASSERT(title);
    JADE_ASSERT(label);
    JADE_ASSERT(bcur_type);
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    // Map BCUR cbor into a series of QR-code icons
    Icon* icons = NULL;
    size_t num_icons = 0;
    const uint8_t qrcode_version = qr_version_from_flags(qr_flags);
    bcur_create_qr_icons(cbor, cbor_len, bcur_type, qrcode_version, &icons, &num_icons);

    // Create qr activity for those icons
    const uint8_t frames_per_qr = qr_animation_speed_from_flags(qr_flags);
    make_show_qr_activity(activity_ptr, title, label, icons, num_icons, frames_per_qr);
    JADE_ASSERT(*activity_ptr);
}

// Display a QR code, with access to size_speed options
static void display_bcur_qr(
    const char* title, const char* label, const char* bcur_type, const uint8_t* cbor, const size_t cbor_len)
{
    JADE_ASSERT(title);
    JADE_ASSERT(label);
    JADE_ASSERT(bcur_type);
    JADE_ASSERT(cbor);
    JADE_ASSERT(cbor_len);

    uint16_t qr_flags = storage_get_qr_flags();

    // Create show psbt activity for those icons
    gui_activity_t* activity = NULL;
    create_display_bcur_qr_activity(&activity, title, label, bcur_type, cbor, cbor_len, qr_flags);
    JADE_ASSERT(activity);

    while (true) {
        // Show, and await button click
        gui_set_current_activity(activity);

        int32_t ev_id;
#ifndef CONFIG_DEBUG_UNATTENDED_CI
        const bool ret = gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
#else
        gui_activity_wait_event(activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, NULL, NULL,
            CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
        const bool ret = true;
        ev_id = BTN_QR_DISPLAY_EXIT;
#endif
        if (ret) {
            if (ev_id == BTN_QR_OPTIONS) {
                if (handle_qr_options(&qr_flags)) {
                    // Options were updated - re-create psbt qr screen
                    display_message_activity("Processing...");
                    create_display_bcur_qr_activity(&activity, title, label, bcur_type, cbor, cbor_len, qr_flags);
                }
            } else if (ev_id == BTN_QR_DISPLAY_EXIT) {
                // Done
                break;
            }
        }
    }
}

// Parse a BC-UR PSBT and attempt to sign and display as BC-UR QR
static bool parse_sign_display_bcur_psbt_qr(const char* type, const uint8_t* data, const size_t data_len)
{
    JADE_ASSERT(type);
    JADE_ASSERT(data);
    JADE_ASSERT(data_len);

    // Parse scanned QR data
    struct wally_psbt* psbt = NULL;
    if (strcasecmp(type, BCUR_TYPE_CRYPTO_PSBT) || !parse_bcur_psbt_cbor(data, data_len, &psbt)) {
        // Unexpected type/format
        await_error_activity("Unsupported QR/PSBT format");
        return false;
    }

    // Try to sign extracted PSBT
    bool ret = false;
    const char* errmsg = NULL;
    const int errcode = sign_psbt(psbt, &errmsg);
    if (errcode) {
        if (errcode != CBOR_RPC_USER_CANCELLED) {
            await_error_activity(errmsg);
        }
        goto cleanup;
    }

    // Build BCUR message holding the signed PSBT
    uint8_t* cbor = NULL;
    size_t cbor_len = 0;
    if (!bcur_build_cbor_crypto_psbt(psbt, &cbor, &cbor_len)) {
        JADE_LOGW("Failed to build bcur/cbor for psbt");
        return false;
    }

    // Now display bcur QR
    display_bcur_qr("PSBT Export", "Scan using\nwallet app", BCUR_TYPE_CRYPTO_PSBT, cbor, cbor_len);

cleanup:
    JADE_WALLY_VERIFY(wally_psbt_free(psbt));
    return ret;
}

// Handle scanning a QR - supports addresses and PSBTs
void handle_scan_qr(void)
{
    // Scan QR - potentially a BC-UR/multi-frame QR
    char* type = NULL;
    uint8_t* data = NULL;
    size_t data_len = 0;
    if (!bcur_scan_qr("Scan address\nor PSBT", &type, &data, &data_len) || !data) {
        // Scan aborted
        return;
    }

    if (type) {
        // BC-UR scanned - try to process as PSBT
        if (!parse_sign_display_bcur_psbt_qr(type, data, data_len)) {
            JADE_LOGE("Processing BC-UR as PSBT failed: %s", type);
        }
    } else {
        // Non-BC-UR (single frame) data - try to process as address
        JADE_ASSERT(data[data_len] == '\0');
        if (!verify_address((const char*)data)) {
            JADE_LOGE("Processing QR as address failed: %s", (const char*)data);
        }
    }

    // In either case we need to free the scanned data
    free(type);
    free(data);
}

// Display screen with help url and qr code
void display_qr_help_screen(const char* url)
{
    JADE_ASSERT(url);

    const size_t url_len = strlen(url);
    JADE_ASSERT(url_len < 78);

    // Create icon for url
    // For sizes, see: https://www.qrcode.com/en/about/version.html - 'Binary'
    const uint8_t qr_version = url_len < 32 ? 2 : 4;
    const uint8_t scale_factor = qr_version == 2 ? 4 : 3;

    // Convert url to qr code, then to Icon
    QRCode qrcode;
    uint8_t qrbuffer[140]; // opaque work area
    JADE_ASSERT(qrcode_getBufferSize(qr_version) <= sizeof(qrbuffer));
    const int qret = qrcode_initText(&qrcode, qrbuffer, qr_version, ECC_LOW, url);
    JADE_ASSERT(qret == 0);

    Icon qr_icon;
    qrcode_toIcon(&qrcode, &qr_icon, scale_factor);

    // Show, and await button click
    gui_activity_t* activity = NULL;
    make_show_qr_help_activity(&activity, url, &qr_icon);
    JADE_ASSERT(activity);
    gui_set_current_activity(activity);

#ifndef CONFIG_DEBUG_UNATTENDED_CI
    gui_activity_wait_event(activity, GUI_BUTTON_EVENT, BTN_EXIT_QR_HELP, NULL, NULL, NULL, 0);
#else
    gui_activity_wait_event(activity, GUI_BUTTON_EVENT, BTN_EXIT_QR_HELP, NULL, NULL, NULL,
        CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
#endif

    // Free the qr code icon
    qrcode_freeIcon(&qr_icon);
}
