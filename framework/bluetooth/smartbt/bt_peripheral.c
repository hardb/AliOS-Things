/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

//#include "aosbt.h"
#include "os_wrapper.h"
#include "StringUtils.h"
#include "smartbt_peripheral.h"

#include "aos_bt_gatt.h"
#include "aos_bt_ble.h"
#include "smartbt_cfg.h"

#include "smartbt.h"
#include "bt_smartbridge_socket_manager.h"
#include "bt_smartbridge_helper.h"
#include "bt_peripheral_stack_interface.h"

/******************************************************
 *                      Macros
 ******************************************************/

/******************************************************
 *                    Constants
 ******************************************************/

#define SOCKET_INVALID_CONNECTION_HANDLE       ( 0xFFFF )

#define MAX_CONNECTION_TIMEOUT                 ( 10000 )

/******************************************************
 *                   Enumerations
 ******************************************************/

/******************************************************
 *                 Type Definitions
 ******************************************************/

/******************************************************
 *                    Structures
 ******************************************************/

/******************************************************
 *               Static Function Declarations
 ******************************************************/

static OSStatus peripheral_app_connection_handler               ( void *arg );
static OSStatus peripheral_app_disconnection_handler            ( void *arg );

/******************************************************
 *               Variable Definitions
 ******************************************************/

extern gatt_subprocedure_t              peripheral_subprocedure;

aos_bt_peripheral_socket_t            *peripheral_socket = NULL;
static aos_bool_t                      initialised       = AOS_FALSE;
extern aos_bool_t                      bt_initialised;
extern aos_bt_dev_ble_io_caps_req_t    local_io_caps_ble;

aos_bt_peripheral_socket_t     *connecting_peripheral_socket = NULL;

/******************************************************
 *               Function Definitions
 ******************************************************/

static void peripheral_gatt_connection_handler( uint16_t connection_handle )
{
    bt_peripheral_log( "GATT connection was SUCCESS" );

    aos_rtos_send_asynchronous_event( AOS_BT_WORKER_THREAD, peripheral_app_connection_handler, (void *)peripheral_socket );
}

static void peripheral_gatt_disconnection_handler( uint16_t connection_handle )
{
    bt_peripheral_log( "GATT disconnection" );

    /* Remove socket from the connected list */
    if ( peripheral_socket->connection_handle != SOCKET_INVALID_CONNECTION_HANDLE ) {
        /* Reset connection handle to invalid value */
        peripheral_socket->connection_handle = SOCKET_INVALID_CONNECTION_HANDLE;

        /* Reset socket state */
        peripheral_socket->state = SOCKET_STATE_DISCONNECTED;

        /* Check if disconnection is from host or remote device */
        if ( peripheral_helper_socket_check_actions_enabled( peripheral_socket, SOCKET_ACTION_HOST_DISCONNECT ) == AOS_TRUE ) {
            /* Disconnection is originated from the host. Notify app thread that disconnection is complete */
            aos_rtos_set_semaphore( &peripheral_socket->semaphore );
        } else {
            /* Notify app that connection is disconnected by the remote device */
            if ( peripheral_socket->disconnection_callback != NULL ) {
                aos_rtos_send_asynchronous_event( AOS_BT_EVT_WORKER_THREAD, peripheral_app_disconnection_handler,
                                                  (void *)peripheral_socket );
            }

            /* If disconnection happens when connection is still being established. Notify app */
            if ( connecting_peripheral_socket == peripheral_socket ) {
                aos_rtos_set_semaphore( &connecting_peripheral_socket->semaphore );
            }
        }
    } else {
        /* If disconnection happens when connection is still being established. Notify app */
        if ( connecting_peripheral_socket != NULL ) {
            aos_rtos_set_semaphore( &connecting_peripheral_socket->semaphore );
        }
    }
}

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
static aos_bt_gatt_status_t peripheral_gatt_read_request_handler( aos_bt_gatt_read_t *request_data )
{
    aos_bt_ext_attribute_value_t *attribute;
    aos_bt_gatt_status_t status = AOS_BT_GATT_SUCCESS;

    // Find characteristic value
    if ( aos_bt_peripheral_ext_attribute_find_by_handle( request_data->handle, &attribute ) == oNoErr ) {
        /* Invoke attribute_handler before real read request, prepare attribute data if needed */
        if ( attribute->attribute_handler != NULL  ) {
            /* Invock callback only once per attribute read */
            if ( request_data->offset == 0 ) { //|| request_data->is_long == AOS_FALSE )
                status = ( attribute->attribute_handler )( attribute, GATTS_REQ_TYPE_READ );
            }
        }

        /* Calculate the number of copied bytes */
        *(request_data->p_val_len) = MIN( attribute->value_length - request_data->offset, *(request_data->p_val_len) );

        /* Copy the attribute value */
        if ( *(request_data->p_val_len) ) {
            memcpy( request_data->p_val, (void *)((uint8_t *)attribute->p_value + request_data->offset),
                    *(request_data->p_val_len) );
        }
    } else {
        status = AOS_BT_GATT_READ_NOT_PERMIT;
    }
    return status;
}

aos_bt_gatt_status_t bt_peripheral_gatt_callback( aos_bt_gatt_evt_t event, aos_bt_gatt_event_data_t *p_event_data )
{
    aos_bt_ext_attribute_value_t *attribute;

    aos_bt_gatt_status_t status = AOS_BT_GATT_SUCCESS;

    switch (event) {
        case GATT_CONNECTION_STATUS_EVT: {
            if ( p_event_data->connection_status.link_role == BT_SMART_LINK_ROLE_SLAVE ) {
                /* Connection */
                if ( p_event_data->connection_status.connected == AOS_TRUE ) {
                    /* Store remote device information */
                    connecting_peripheral_socket = peripheral_socket;
                    memcpy( &peripheral_socket->remote_device.address, p_event_data->connection_status.bd_addr, BD_ADDR_LEN );
                    peripheral_socket->remote_device.address_type = (aos_bt_smart_address_type_t)p_event_data->connection_status.addr_type;
                    peripheral_socket->connection_handle = p_event_data->connection_status.conn_id;
                    peripheral_socket->state = SOCKET_STATE_LINK_CONNECTED;
                    peripheral_gatt_connection_handler( p_event_data->connection_status.conn_id );
                } else {
                    peripheral_gatt_disconnection_handler( p_event_data->connection_status.conn_id );
                }
            }
            break;
        }

        case GATT_ATTRIBUTE_REQUEST_EVT: {
            /* GATT attribute read/write request */
            if ( p_event_data->attribute_request.request_type == GATTS_REQ_TYPE_WRITE ) {
                if ( p_event_data->attribute_request.data.write_req.is_prep == AOS_TRUE ) {
                    bt_peripheral_log("Not implement");
                    break;
                }

                if ( aos_bt_peripheral_ext_attribute_find_by_handle( p_event_data->attribute_request.data.handle,
                                                                     &attribute ) == oNoErr ) {
                    aos_bt_peripheral_ext_attribute_value_write( attribute, p_event_data->attribute_request.data.write_req.val_len,
                                                                 p_event_data->attribute_request.data.write_req.offset, p_event_data->attribute_request.data.write_req.p_val );

                    if ( attribute->attribute_handler != NULL ) {
                        status = (attribute->attribute_handler)( attribute, GATTS_REQ_TYPE_WRITE );
                    }
                } else {
                    status = AOS_BT_GATT_WRITE_NOT_PERMIT;
                }

                break;
            } else if ( p_event_data->attribute_request.request_type == GATTS_REQ_TYPE_WRITE_EXEC ) {
                bt_peripheral_log("Not implement");
                break;
            } else if ( p_event_data->attribute_request.request_type == GATTS_REQ_TYPE_READ) {
                status = peripheral_gatt_read_request_handler( &p_event_data->attribute_request.data.read_req );
                break;
            } else if ( p_event_data->attribute_request.request_type == GATTS_REQ_TYPE_MTU ) {
                bt_peripheral_log("GATT Event: GATTS_REQ_TYPE_MTU, mtu = %d", p_event_data->attribute_request.data.mtu);
                \
                break;
            } else if ( p_event_data->attribute_request.request_type == GATTS_REQ_TYPE_CONF ) {
                subprocedure_notify_complete( &peripheral_subprocedure );
                break;
            }
        }
        default: {
            bt_smartbridge_log( "Gatt callback event:%d", event );
            break;
        }

    }

    return status;
}

OSStatus aos_bt_peripheral_init(   aos_bt_peripheral_socket_t                   *socket,
                                   const aos_bt_smart_security_settings_t       *settings,
                                   aos_bt_peripheral_connection_callback_t       connection_callback,
                                   aos_bt_peripheral_disconnection_callback_t    disconnection_callback,
                                   aos_bt_smart_bonding_callback_t               bonding_callback )
{
    OSStatus result;

    if ( initialised == AOS_TRUE ) {
        return AOS_BT_SUCCESS;
    }

    bt_peripheral_log( "Initialising AOS Smart preipheral ..." );

    /* Reset socket fields */
    memset( socket, 0, sizeof( *socket ) );
    socket->connection_handle = SOCKET_INVALID_CONNECTION_HANDLE;

    /* Initialise socket semaphore */
    result = aos_rtos_init_semaphore( &socket->semaphore, 1 );
    require_noerr(result, exit);

    /* Initialise callbacks */
    socket->connection_callback = connection_callback;
    socket->disconnection_callback = disconnection_callback;
    socket->bonding_callback = bonding_callback;

    /* Reset connection handle to invalid value */
    socket->connection_handle = SOCKET_INVALID_CONNECTION_HANDLE;

    /* Reset state */
    socket->state = SOCKET_STATE_DISCONNECTED;

    /* Create service linked-list */
    result = linked_list_init( &socket->attribute_database );
    require_noerr(result, exit);

    /* Set local copies of security settings */
    memcpy( &socket->security_settings, settings, sizeof( *settings ) );

    peripheral_bt_interface_set_security_settings( settings );

    /* Enable security */
    if (settings && bonding_callback && settings->authentication_requirements != BT_SMART_AUTH_REQ_NONE) {
        peripheral_helper_socket_set_actions(socket, SOCKET_ACTION_INITIATE_PAIRING);
    } else {
        peripheral_helper_socket_clear_actions(socket, SOCKET_ACTION_INITIATE_PAIRING);
    }

    peripheral_socket = socket;

    /* Initialise bt stack operation interface */
    //TODO use a different interface!!!
    peripheral_bt_interface_initialize();

    initialised = AOS_TRUE;
exit:
    return result;
}

OSStatus aos_bt_peripheral_deinit( void )
{
    if ( initialised == AOS_FALSE ) {
        return AOS_BT_SUCCESS;
    }

    peripheral_socket = NULL;

    /* Uninitialise bt stack operation interface */
    //TODO use a different interface!!!
    peripheral_bt_interface_deinitialize();

    initialised = AOS_FALSE;

    return AOS_BT_SUCCESS;
}

OSStatus aos_bt_peripheral_delete_socket( aos_bt_peripheral_socket_t *socket )
{
    OSStatus result;
    if ( initialised == AOS_FALSE ) {
        return AOS_BT_SMART_APPL_UNINITIALISED;
    }

    result = aos_rtos_deinit_semaphore( &socket->semaphore );
    if ( result != AOS_BT_SUCCESS ) {
        return result;
    }

    memset( socket, 0, sizeof( *socket ) );
    socket->connection_handle = SOCKET_INVALID_CONNECTION_HANDLE;
    return AOS_BT_SUCCESS;
}

OSStatus aos_bt_peripheral_disconnect( void )
{
    if ( initialised == AOS_FALSE ) {
        return AOS_BT_SMART_APPL_UNINITIALISED;
    }

    /* Mark disconnection flag that it's coming from the host */
    peripheral_helper_socket_set_actions( peripheral_socket, SOCKET_ACTION_HOST_DISCONNECT );

    /* Clean-up accidentally set semaphores */
    while ( aos_rtos_get_semaphore( &peripheral_socket->semaphore, AOS_NO_WAIT ) == AOS_BT_SUCCESS ) {
    }

    /* Check if either link is encrypted or connected */
    if ( peripheral_socket->state >= SOCKET_STATE_LINK_CONNECTED ) {
        peripheral_bt_interface_disconnect( peripheral_socket->connection_handle );
        /* Wait for disconnection */
        aos_rtos_get_semaphore( &peripheral_socket->semaphore, 5 * 1000 );
    } else {
        /* Link is not yet connected. Cancel last */
        peripheral_bt_interface_cancel_last_connect( peripheral_socket->remote_device.address );
    }

    /* Clear socket disconnect action */
    peripheral_helper_socket_set_actions( peripheral_socket, SOCKET_ACTION_HOST_DISCONNECT );

    /* Proper clean-up if socket isn't properly disconnected */
    if ( peripheral_socket->state != SOCKET_STATE_DISCONNECTED ) {
        /* Reset connection handle to invalid value */
        peripheral_socket->connection_handle = SOCKET_INVALID_CONNECTION_HANDLE;

        /* Clear socket state */
        peripheral_socket->state = SOCKET_STATE_DISCONNECTED;
    }

    return AOS_BT_SUCCESS;
}

OSStatus aos_bt_peripheral_gatt_indicate_attribute_value ( aos_bt_peripheral_socket_t *socket,
                                                           const aos_bt_ext_attribute_value_t *attribute )
{
    aos_bt_peripheral_socket_status_t status;

    if ( initialised == AOS_FALSE ) {
        return AOS_BT_SMART_APPL_UNINITIALISED;
    }

    if ( socket == NULL || attribute == NULL ) {
        return AOS_BT_BADARG;
    }

    aos_bt_peripheral_get_socket_status( socket, &status );
    if ( status != PERIPHERAL_SOCKET_CONNECTED ) {
        return AOS_BT_SOCKET_NOT_CONNECTED;
    }

    return peripheral_bt_interface_indicate_attribute_value( socket->connection_handle, attribute );
}


OSStatus aos_bt_peripheral_gatt_notify_attribute_value ( aos_bt_peripheral_socket_t *socket,
                                                         const aos_bt_ext_attribute_value_t *attribute )
{
    aos_bt_peripheral_socket_status_t status;

    if ( initialised == AOS_FALSE ) {
        return AOS_BT_SMART_APPL_UNINITIALISED;
    }

    if ( socket == NULL || attribute == NULL ) {
        return AOS_BT_BADARG;
    }

    aos_bt_peripheral_get_socket_status( socket, &status );
    if ( status != PERIPHERAL_SOCKET_CONNECTED ) {
        return AOS_BT_SOCKET_NOT_CONNECTED;
    }

    return peripheral_bt_interface_notify_attribute_value( socket->connection_handle, attribute );
}





OSStatus aos_bt_peripheral_start_advertisements( aos_bt_smart_advertising_settings_t *settings,
                                                 aos_bt_smart_advertising_complete_callback_t complete_callback)
{
    return peripheral_bt_interface_start_advertisements( settings, complete_callback );
}

OSStatus aos_bt_peripheral_stop_advertisements( void )
{
    return peripheral_bt_interface_stop_advertisements();
}

OSStatus aos_bt_peripheral_update_advertisements_white_list( aos_bool_t add, aos_bt_device_address_t device_address )
{
    if ( initialised == AOS_FALSE ) {
        return oNotInitializedErr;
    }
    return peripheral_bt_interface_update_advertisements_white_list( add, device_address );
}

OSStatus aos_bt_peripheral_get_advertisements_white_list_size( uint8_t *size )
{
    if ( initialised == AOS_FALSE ) {
        return oNotInitializedErr;
    }
    if ( size == (uint8_t *)0 ) {
        return oParamErr;
    }
    return peripheral_bt_interface_get_advertisements_white_list_size( size );
}

OSStatus aos_bt_peripheral_set_advertisements_filter_policy( aos_bt_peripheral_adv_filter_policy_t policy )
{
    if ( initialised == AOS_FALSE ) {
        return oNotInitializedErr;
    }
    return peripheral_bt_interface_set_advertisements_filter_policy( policy );
}

OSStatus aos_bt_peripheral_get_socket_status( aos_bt_peripheral_socket_t *socket,
                                              aos_bt_peripheral_socket_status_t *status )
{
    if ( initialised == AOS_FALSE ) {
        return AOS_BT_SMART_APPL_UNINITIALISED;
    }

    if ( socket->state == SOCKET_STATE_LINK_ENCRYPTED ) {
        *status = PERIPHERAL_SOCKET_CONNECTED;
    } else if ( socket->state == SOCKET_STATE_LINK_CONNECTED ) {
        /* Status is connected if socket does not have loaded bond info and does not initiate pairing */
        // if ( smartbridge_helper_socket_check_actions_disabled( server_socket, SOCKET_ACTION_ENCRYPT_USING_BOND_INFO | SOCKET_ACTION_INITIATE_PAIRING ) == AOS_TRUE )
        // {
        *status = PERIPHERAL_SOCKET_CONNECTED;
        // }
        // else
        // {
        //     *status = Smart Peripheral_SOCKET_CONNECTING;
        // }
    } else {
        *status = PERIPHERAL_SOCKET_DISCONNECTED;
    }

    return AOS_BT_SUCCESS;
}

static OSStatus bt_peripheral_get_buffer( void **buffer, uint32_t size )
{
    OSStatus err = oNoErr;
    require_action( buffer != NULL, exit, err = oParamErr );
    /* Allocate buffer object */
    *buffer = malloc( size );
    require_action( *buffer != NULL, exit, err = oNoMemoryErr);
exit:
    return err;
}

static OSStatus bt_peripheral_resize_buffer( void **buffer, uint32_t size )
{
    OSStatus err = oNoErr;
    require_action( buffer != NULL, exit, err = oParamErr );
    /* Allocate buffer object */
    *buffer = realloc( *buffer, size );
    require_action( *buffer != NULL, exit, err = oNoMemoryErr);
exit:
    return err;
}

static OSStatus bt_peripheral_release_buffer( void *buffer )
{
    OSStatus err = oNoErr;
    require_action( buffer != NULL, exit, err = oParamErr );
    free( buffer );
exit:
    return err;
}

aos_bt_ext_attribute_value_t *aos_bt_peripheral_ext_attribute_add( uint16_t handle, uint16_t value_length,
                                                                   const uint8_t *value, aos_bt_peripheral_attribute_handler handler )
{
    aos_bt_ext_attribute_value_t *new_attribite = NULL;
    OSStatus err = oNoErr;
    void          *value_buffer;

    require_action( initialised == AOS_TRUE, exit, err = oNotInitializedErr );

    /* Get buffer */
    err = bt_peripheral_get_buffer( (void **)&new_attribite, sizeof( aos_bt_ext_attribute_value_t ) );
    require_noerr(err, exit);

    /* Copy content to buffer */
    new_attribite->handle                           = handle;
    new_attribite->value_length                     = 0;
    new_attribite->value_buffer_length              = 0;
    new_attribite->p_value                          = NULL;
    new_attribite->attribute_handler                = NULL;

    /* Allocate buffer for value */
    if ( value_length != 0) {
        err = bt_peripheral_get_buffer( (void **)&value_buffer, value_length );
        require_noerr(err, exit);

        memcpy( value_buffer, (void *)value, value_length );
        new_attribite->p_value                      = value_buffer;
        new_attribite->value_length                 = value_length;
        new_attribite->value_buffer_length          = value_length;
    }

    if ( handler != NULL ) {
        new_attribite->attribute_handler = handler;
    }

    /* Add to socket */
    err = linked_list_insert_node_at_rear( &peripheral_socket->attribute_database, &new_attribite->this_node );
    require_noerr_action(err, exit, bt_peripheral_release_buffer( (void *)new_attribite ));

exit:
    if ( err != oNoErr && new_attribite ) {
        aos_bt_peripheral_ext_attribute_remove( new_attribite );
        new_attribite = NULL;
    }
    return new_attribite;
}

OSStatus aos_bt_peripheral_ext_attribute_remove( aos_bt_ext_attribute_value_t *attribute )
{
    OSStatus err = oNoErr;

    require_action( initialised == AOS_TRUE, exit, err = oNotInitializedErr );

    /* Remove characteristic from service */
    err = linked_list_remove_node( &peripheral_socket->attribute_database, &attribute->this_node );
    require_noerr( err, exit );

    /* Delete value */
    if ( attribute->p_value != NULL ) {
        err = bt_peripheral_release_buffer( (void *)attribute->p_value );
        require_noerr(err, exit);
    }

    /* Delete characteristic */
    err = bt_peripheral_release_buffer( (void *)attribute );
    require_noerr(err, exit);

exit:
    return err;
}

OSStatus aos_bt_peripheral_ext_attribute_value_write( aos_bt_ext_attribute_value_t *attribute, uint16_t length,
                                                      uint16_t offset, const uint8_t *value )
{
    OSStatus err = oNoErr;

    require_action( initialised == AOS_TRUE, exit, err = oNotInitializedErr );

    if ( attribute->p_value == NULL ) {
        /* Allocate buffer for value */
        err = bt_peripheral_get_buffer( (void **)&attribute->p_value, length + offset );
        require_noerr(err, exit);
        attribute->value_buffer_length = length + offset;
    } else {
        if ( ( length + offset ) > attribute->value_buffer_length ) {
            err = bt_peripheral_resize_buffer( (void **)&attribute->p_value, length + offset );
            require_noerr(err, exit);
            attribute->value_buffer_length = length + offset;
        }
    }

    memcpy( attribute->p_value + offset , (void *)value, length );
    attribute->value_length = length + offset;

exit:
    return err;
}

static bool compare_attribute_by_handle( linked_list_node_t *node_to_compare, void *user_data )
{
    aos_bt_ext_attribute_value_t *current_attribute = (aos_bt_ext_attribute_value_t *)node_to_compare;
    uint16_t               attribute_handle  = (uint16_t)( (uint32_t)user_data & 0xffff );

    if ( current_attribute->handle == attribute_handle ) {
        return true;
    } else {
        return false;
    }
}

OSStatus aos_bt_peripheral_ext_attribute_find_by_handle( uint16_t handle,
                                                         aos_bt_ext_attribute_value_t **attribute_found )
{
    OSStatus err = oNoErr;

    require_action( ( initialised == AOS_TRUE ) && ( attribute_found != NULL ), exit, err = oParamErr );

    err = linked_list_find_node( &peripheral_socket->attribute_database, compare_attribute_by_handle,
                                 (void *)( handle & 0xffffffff ), (linked_list_node_t **)attribute_found );

exit:
    return err;
}

/******************************************************
 *               Callback Definitions
 ******************************************************/

static OSStatus peripheral_app_connection_handler( void *arg )
{
    aos_bt_peripheral_socket_t *socket = (aos_bt_peripheral_socket_t *)arg;

    /* Performing PAIRING & ENCRYPTION Procedure */
    if (peripheral_helper_socket_check_actions_enabled(socket, SOCKET_ACTION_INITIATE_PAIRING)) {
        if (aos_bt_dev_find_bonded_device(socket->remote_device.address)) {
            aos_bt_start_encryption(&socket->remote_device.address);
        } else {
            aos_bt_start_pairing(socket->remote_device.address, socket->remote_device.address_type, &socket->security_settings);
            aos_rtos_get_semaphore(&socket->semaphore, AOS_NEVER_TIMEOUT);
            aos_bt_start_encryption(&socket->remote_device.address);
        }

        aos_rtos_get_semaphore(&socket->semaphore, AOS_NEVER_TIMEOUT);
    }

    /* Finished */
    connecting_peripheral_socket = NULL;

    if ( socket != NULL && socket->connection_callback != NULL ) {
        return aos_rtos_send_asynchronous_event( AOS_BT_EVT_WORKER_THREAD,
                                                 (event_handler_t)socket->connection_callback,
                                                 (void *)socket );
    }

    return AOS_BT_ERROR;
}


static OSStatus peripheral_app_disconnection_handler( void *arg )
{
    aos_bt_peripheral_socket_t *socket = (aos_bt_peripheral_socket_t *)arg;

    if ( socket != NULL && socket->disconnection_callback != NULL ) {
        socket->disconnection_callback( socket );
        return AOS_BT_SUCCESS;
    }

    return AOS_BT_ERROR;
}
