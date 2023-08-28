#include "flash_manager.h"
#include <cstring>

#define ROUND_UP(value, boundary) ((value) + ((boundary) - (value)) % (boundary))
#define ROUND_DOWN(value, boundary) ((value) - ((value) % (boundary)))

FlashManager::FlashManager()
    : _flash_intf(nullptr),
      _flash_state(FLASH_STATE_CLOSED),
      _current_sector_valid(false),
      _last_packet_addr(0),
      _current_write_block_addr(0),
      _current_write_block_size(0),
      _current_sector_addr(0),
      _current_sector_size(0),
      _page_buf_empty(true)
{
    memset(_page_buffer, 0xff, sizeof(_page_buffer));
}

error_t FlashManager::flush_current_block(uint32_t addr)
{
    error_t status = ERROR_SUCCESS;

    // Write out current buffer if there is data in it
    if (!_page_buf_empty)
    {
        status = _flash_intf->program_page(_current_write_block_addr, _page_buffer, _current_write_block_size);
        _page_buf_empty = true;
    }

    // Setup for next block
    memset(_page_buffer, 0xFF, _current_write_block_size);
    _current_write_block_addr = ROUND_DOWN(addr, _current_write_block_size);

    return status;
}

error_t FlashManager::setup_next_sector(uint32_t addr)
{
    uint32_t min_prog_size;
    uint32_t sector_size;
    error_t status;

    min_prog_size = _flash_intf->program_page_min_size(addr);
    sector_size = _flash_intf->erase_sector_size(addr);

    if ((min_prog_size <= 0) || (sector_size <= 0))
    {
        return ERROR_INTERNAL;
    }

    // Setup global variables
    _current_sector_addr = ROUND_DOWN(addr, sector_size);
    _current_sector_size = sector_size;
    _current_write_block_addr = _current_sector_addr;
    _current_write_block_size = (sector_size <= sizeof(_page_buffer)) ? (sector_size) : (sizeof(_page_buffer));

    // check flash algo every sector change, addresses with different flash algo should be sector aligned
    if (_flash_intf->flash_algo_set)
    {
        status = _flash_intf->flash_algo_set(_current_sector_addr);
        if (ERROR_SUCCESS != status)
        {
            _flash_intf->uninit();
            return status;
        }
    }

    // Erase the current sector
    status = _flash_intf->erase_sector(_current_sector_addr);
    if (ERROR_SUCCESS != status)
    {
        _flash_intf->uninit();
        return status;
    }

    // Clear out buffer in case block size changed
    memset(_page_buffer, 0xFF, _current_write_block_size);

    return ERROR_SUCCESS;
}

bool FlashManager::flash_intf_valid(flash_intf_t *flash_intf)
{
    if (0 == flash_intf)
    {
        return false;
    }

    if (0 == flash_intf->uninit)
    {
        return false;
    }

    if (0 == flash_intf->program_page)
    {
        return false;
    }

    if (0 == flash_intf->erase_sector)
    {
        return false;
    }

    if (0 == flash_intf->erase_chip)
    {
        return false;
    }

    if (0 == flash_intf->program_page_min_size)
    {
        return false;
    }

    if (0 == flash_intf->erase_sector_size)
    {
        return false;
    }

    if (0 == flash_intf->flash_busy)
    {
        return false;
    }

    return true;
}

error_t FlashManager::init(flash_intf_t *flash_intf)
{
    error_t status = ERROR_SUCCESS;

    if (_flash_state != FLASH_STATE_CLOSED)
    {
        return ERROR_INTERNAL;
    }

    // Check for a valid flash interface
    if (!flash_intf_valid(flash_intf))
    {
        return ERROR_INTERNAL;
    }

    // Initialize variables
    memset(_page_buffer, 0xFF, sizeof(_page_buffer));
    _page_buf_empty = true;
    _current_sector_valid = false;
    _current_write_block_addr = 0;
    _current_write_block_size = 0;
    _current_sector_addr = 0;
    _current_sector_size = 0;
    _last_packet_addr = 0;
    _flash_intf = flash_intf;

    // Initialize flash
    status = _flash_intf->init();
    if (ERROR_SUCCESS != status)
    {
        return status;
    }

    _flash_state = FLASH_STATE_OPEN;

    return status;
}

error_t FlashManager::write(uint32_t packet_addr, const uint8_t *data, uint32_t size)
{
    uint32_t page_buf_left = 0;
    uint32_t copy_size = 0;
    uint32_t copy_start_pos = 0;
    error_t status = ERROR_SUCCESS;

    if (_flash_state != FLASH_STATE_OPEN)
    {
        return ERROR_INTERNAL;
    }

    // Setup the current sector if it is not setup already
    if (!_current_sector_valid)
    {
        status = setup_next_sector(packet_addr);

        if (ERROR_SUCCESS != status)
        {
            _flash_state = FLASH_STATE_ERROR;
            return status;
        }
        _current_sector_valid = true;
        _last_packet_addr = packet_addr;
    }

    // non-increasing address support
    if (ROUND_DOWN(packet_addr, _current_write_block_size) != ROUND_DOWN(_last_packet_addr, _current_write_block_size))
    {
        status = flush_current_block(packet_addr);
        if (ERROR_SUCCESS != status)
        {
            _flash_state = FLASH_STATE_ERROR;
            return status;
        }
    }

    if (ROUND_DOWN(packet_addr, _current_sector_size) != ROUND_DOWN(_last_packet_addr, _current_sector_size))
    {
        status = setup_next_sector(packet_addr);
        if (ERROR_SUCCESS != status)
        {
            _flash_state = FLASH_STATE_ERROR;
            return status;
        }
    }

    while (true)
    {
        // flush if necessary
        if (packet_addr >= _current_write_block_addr + _current_write_block_size)
        {
            status = flush_current_block(packet_addr);

            if (ERROR_SUCCESS != status)
            {
                _flash_state = FLASH_STATE_ERROR;
                return status;
            }
        }

        // Check for end
        if (size <= 0)
        {
            break;
        }

        // Change sector if necessary
        if (packet_addr >= _current_sector_addr + _current_sector_size)
        {
            status = setup_next_sector(packet_addr);

            if (ERROR_SUCCESS != status)
            {
                _flash_state = FLASH_STATE_ERROR;
                return status;
            }
        }

        // write buffer
        copy_start_pos = packet_addr - _current_write_block_addr;
        page_buf_left = _current_write_block_size - copy_start_pos;
        copy_size = ((size) < (page_buf_left) ? (size) : (page_buf_left));
        memcpy(_page_buffer + copy_start_pos, data, copy_size);
        _page_buf_empty = (copy_size == 0);

        // Update variables
        packet_addr += copy_size;
        data += copy_size;
        size -= copy_size;
    }

    _last_packet_addr = packet_addr;

    return status;
}

error_t FlashManager::uninit()
{
    error_t flash_write_ret = ERROR_SUCCESS;
    error_t flash_uninit_ret = ERROR_SUCCESS;

    if (FLASH_STATE_CLOSED == _flash_state)
    {
        return ERROR_INTERNAL;
    }

    // Flush last buffer if its not empty
    if (FLASH_STATE_OPEN == _flash_state)
    {
        flash_write_ret = flush_current_block(0);
    }

    // Close flash interface (even if there was an error during program_page)
    flash_uninit_ret = _flash_intf->uninit();

    // Reset variables to catch accidental use
    memset(_page_buffer, 0xFF, sizeof(_page_buffer));

    _page_buf_empty = true;
    _current_sector_valid = false;
    _current_write_block_addr = 0;
    _current_write_block_size = 0;
    _current_sector_addr = 0;
    _current_sector_size = 0;
    _last_packet_addr = 0;
    _flash_state = FLASH_STATE_CLOSED;

    // Make sure an error from a page write or from an uninit gets propagated
    if (flash_uninit_ret != ERROR_SUCCESS)
    {
        return flash_uninit_ret;
    }

    if (flash_write_ret != ERROR_SUCCESS)
    {
        return flash_write_ret;
    }

    return ERROR_SUCCESS;
}