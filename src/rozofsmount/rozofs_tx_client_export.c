/*
  Copyright (c) 2010 Fizians SAS. <http://www.fizians.com>
  This file is part of Rozofs.

  Rozofs is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation, version 2.

  Rozofs is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
 */

#include <rozofs/rpc/storcli_proto.h>

#include "rozofs_fuse_api.h"
#include "rozofs_rw_load_balancing.h"

/**
* API for creation a transaction towards an exportd

 The reference of the north load balancing is extracted for the client structure
 fuse_ctx_p:
 That API needs the pointer to the current fuse context. That nformation will be
 saved in the transaction context as userParam. It is intended to be used later when
 the client gets the response from the server
 encoding function;
 For making that API generic, the caller is intended to provide the function that
 will encode the message in XDR format. The source message that is encoded is 
 supposed to be pointed by msg2encode_p.
 Since the service is non-blocking, the caller MUST provide the callback function 
 that will be used for decoding the message
 

 @param clt        : pointer to the client structure
 @param timeout_sec : transaction timeout
 @param prog       : program
 @param vers       : program version
 @param opcode     : metadata opcode
 @param encode_fct : encoding function
 @msg2encode_p     : pointer to the message to encode
 @param recv_cbk   : receive callback function
 @param fuse_ctx_p : pointer to the fuse context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */

int rozofs_export_send_common(exportclt_t * clt,uint32_t timeout_sec,uint32_t prog,uint32_t vers,
                              int opcode,xdrproc_t encode_fct,void *msg2encode_p,
                              sys_recv_pf_t recv_cbk,void *fuse_ctx_p) 
{
    DEBUG_FUNCTION;
   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    void              *xmit_buf = NULL;
    int               bufsize;
    int               ret;
    int               position;
    XDR               xdrs;    
	struct rpc_msg   call_msg;
    uint32_t         null_val = 0;

    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    }    
    /*
    ** allocate an xmit buffer
    */  
    xmit_buf = ruc_buf_getBuffer(ROZOFS_TX_SMALL_TX_POOL);
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
    arg_p = (uint8_t*)(header_size_p+1);  
    /*
    ** create the xdr_mem structure for encoding the message
    */
    bufsize = rozofs_tx_get_small_buffer_size();
    xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
	call_msg.rm_xid             = rozofs_tx_alloc_xid(rozofs_tx_ctx_p); 
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	/* XXX: prog and vers have been long historically :-( */
	call_msg.rm_call.cb_prog = (uint32_t)prog;
	call_msg.rm_call.cb_vers = (uint32_t)vers;
	if (! xdr_callhdr(&xdrs, &call_msg))
    {
       /*
       ** THIS MUST NOT HAPPEN
       */
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;	
    }
    /*
    ** insert the procedure number, NULL credential and verifier
    */
    XDR_PUTINT32(&xdrs, (int32_t *)&opcode);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
        
    /*
    ** ok now call the procedure to encode the message
    */
    if ((*encode_fct)(&xdrs,msg2encode_p) == FALSE)
    {
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
    /*
    ** update the length of the message : must be in network order
    */
    *header_size_p = htonl(0x80000000 | position);
    /*
    ** set the payload length in the xmit buffer
    */
    int total_len = sizeof(*header_size_p)+ position;
    ruc_buf_setPayloadLen(xmit_buf,total_len);
    /*
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    rozofs_tx_ctx_p->user_param = fuse_ctx_p;    
    /*
    ** now send the message
    */
    ret = north_lbg_send(clt->rpcclt.lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p, timeout_sec);
//    if (*tx_ptr != NULL) *tx_ptr = rozofs_tx_ctx_p;
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
//    if (xmit_buf != NULL) ruc_buf_freeBuffer(xmit_buf);    
    return -1;    
}



int rozofs_expgateway_send_common(int lbg_id,uint32_t prog,uint32_t vers,
                              int opcode,xdrproc_t encode_fct,void *msg2encode_p,
                              sys_recv_pf_t recv_cbk,void *fuse_ctx_p) 
{
    DEBUG_FUNCTION;
   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    void              *xmit_buf = NULL;
    int               bufsize;
    int               ret;
    int               position;
    XDR               xdrs;    
	struct rpc_msg   call_msg;
    uint32_t         null_val = 0;

    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    }    
    /*
    ** allocate an xmit buffer
    */  
    xmit_buf = ruc_buf_getBuffer(ROZOFS_TX_SMALL_TX_POOL);
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
    arg_p = (uint8_t*)(header_size_p+1);  
    /*
    ** create the xdr_mem structure for encoding the message
    */
    bufsize = rozofs_tx_get_small_buffer_size();
    xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
	call_msg.rm_xid             = rozofs_tx_alloc_xid(rozofs_tx_ctx_p); 
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	/* XXX: prog and vers have been long historically :-( */
	call_msg.rm_call.cb_prog = (uint32_t)prog;
	call_msg.rm_call.cb_vers = (uint32_t)vers;
	if (! xdr_callhdr(&xdrs, &call_msg))
    {
       /*
       ** THIS MUST NOT HAPPEN
       */
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;	
    }
    /*
    ** insert the procedure number, NULL credential and verifier
    */
    XDR_PUTINT32(&xdrs, (int32_t *)&opcode);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
        
    /*
    ** ok now call the procedure to encode the message
    */
    if ((*encode_fct)(&xdrs,msg2encode_p) == FALSE)
    {
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
    /*
    ** update the length of the message : must be in network order
    */
    *header_size_p = htonl(0x80000000 | position);
    /*
    ** set the payload length in the xmit buffer
    */
    int total_len = sizeof(*header_size_p)+ position;
    ruc_buf_setPayloadLen(xmit_buf,total_len);
    /*
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    rozofs_tx_ctx_p->user_param = fuse_ctx_p;    
    /*
    ** now send the message
    */
    ret = north_lbg_send(lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p, ROZOFS_TMR_GET(TMR_EXPORT_PROGRAM));
//    if (*tx_ptr != NULL) *tx_ptr = rozofs_tx_ctx_p;
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
//    if (xmit_buf != NULL) ruc_buf_freeBuffer(xmit_buf);    
    return -1;    
}


/**
* API for creation a transaction towards an exportd

 The reference of the north load balancing is extracted for the client structure
 fuse_ctx_p:
 That API needs the pointer to the current fuse context. That nformation will be
 saved in the transaction context as userParam. It is intended to be used later when
 the client gets the response from the server
 encoding function;
 For making that API generic, the caller is intended to provide the function that
 will encode the message in XDR format. The source message that is encoded is 
 supposed to be pointed by msg2encode_p.
 Since the service is non-blocking, the caller MUST provide the callback function 
 that will be used for decoding the message
 

 @param eid        : export id
 @param fid        : unique file id (directory, regular file, etc...)
 @param prog       : program
 @param vers       : program version
 @param opcode     : metadata opcode
 @param encode_fct : encoding function
 @msg2encode_p     : pointer to the message to encode
 @param recv_cbk   : receive callback function
 @param fuse_buffer_ctx_p : pointer to the fuse context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */
int rozofs_expgateway_send_routing_common(uint32_t eid,fid_t fid,uint32_t prog,uint32_t vers,
                              int opcode,xdrproc_t encode_fct,void *msg2encode_p,
                              sys_recv_pf_t recv_cbk,void *fuse_buffer_ctx_p) 
{
    DEBUG_FUNCTION;
   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    void              *xmit_buf = NULL;
    int               bufsize;
    int               ret;
    int               position;
    XDR               xdrs;    
	struct rpc_msg   call_msg;
    uint32_t         null_val = 0;
    int lbg_id;

    
    rozofs_fuse_save_ctx_t *fuse_ctx_p;
    
    GET_FUSE_CTX_P(fuse_ctx_p,fuse_buffer_ctx_p);
    
    expgw_tx_routing_ctx_t  *routing_ctx_p = &fuse_ctx_p->expgw_routing_ctx ;
    /*
    ** get the available load balancing group(s) for routing the request 
    */    
    ret  = expgw_get_export_routing_lbg_info(eid,fid,routing_ctx_p);
    if (ret < 0)
    {
      /*
      ** no load balancing group available
      */
      errno = EPROTO;
      goto error;    
    }

    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    }    
    /*
    ** allocate an xmit buffer
    */  
    xmit_buf = ruc_buf_getBuffer(ROZOFS_TX_SMALL_TX_POOL);
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** The system attempts first to forward the message toward load balancing group
    ** of an export gateway and then to the master export if the load balancing group
    ** of the export gateway is not available
    */
    lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
    arg_p = (uint8_t*)(header_size_p+1);  
    /*
    ** create the xdr_mem structure for encoding the message
    */
    bufsize = rozofs_tx_get_small_buffer_size();
    xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
	call_msg.rm_xid             = rozofs_tx_alloc_xid(rozofs_tx_ctx_p); 
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	/* XXX: prog and vers have been long historically :-( */
	call_msg.rm_call.cb_prog = (uint32_t)prog;
	call_msg.rm_call.cb_vers = (uint32_t)vers;
	if (! xdr_callhdr(&xdrs, &call_msg))
    {
       /*
       ** THIS MUST NOT HAPPEN
       */
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;	
    }
    /*
    ** insert the procedure number, NULL credential and verifier
    */
    XDR_PUTINT32(&xdrs, (int32_t *)&opcode);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
        
    /*
    ** ok now call the procedure to encode the message
    */
    if ((*encode_fct)(&xdrs,msg2encode_p) == FALSE)
    {
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
    /*
    ** update the length of the message : must be in network order
    */
    *header_size_p = htonl(0x80000000 | position);
    /*
    ** set the payload length in the xmit buffer
    */
    int total_len = sizeof(*header_size_p)+ position;
    ruc_buf_setPayloadLen(xmit_buf,total_len);
    /*
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    rozofs_tx_ctx_p->user_param = fuse_buffer_ctx_p;    
    /*
    ** now send the message
    */
reloop:
    ret = north_lbg_send(lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       /*
       ** attempt to get the next available load balancing group
       */
       lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
       if (lbg_id >= 0) goto reloop;
       
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p, ROZOFS_TMR_GET(TMR_EXPORT_PROGRAM));
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}





/**
* API for creation a transaction towards an exportd

 The reference of the north load balancing is extracted for the client structure
 fuse_ctx_p:
 That API needs the pointer to the current fuse context. That nformation will be
 saved in the transaction context as userParam. It is intended to be used later when
 the client gets the response from the server
 encoding function;
 For making that API generic, the caller is intended to provide the function that
 will encode the message in XDR format. The source message that is encoded is 
 supposed to be pointed by msg2encode_p.
 Since the service is non-blocking, the caller MUST provide the callback function 
 that will be used for decoding the message
 

 @param rozofs_tx_ctx_p        : transaction context
 @param recv_cbk        : callback function (may be NULL)
 @param fuse_buffer_ctx_p       : buffer containing the fuse context
 @param vers       : program version
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */
int rozofs_expgateway_resend_routing_common(rozofs_tx_ctx_t *rozofs_tx_ctx_p, sys_recv_pf_t recv_cbk,void *fuse_buffer_ctx_p) 
{
    DEBUG_FUNCTION;
   
    void              *xmit_buf = NULL;
    int               ret;
    int lbg_id;    
    rozofs_fuse_save_ctx_t *fuse_ctx_p;
    
    GET_FUSE_CTX_P(fuse_ctx_p,fuse_buffer_ctx_p);    
    expgw_tx_routing_ctx_t  *routing_ctx_p = &fuse_ctx_p->expgw_routing_ctx ;


    /*
    ** get the xmit buffer from the current routing context
    */  
    xmit_buf = routing_ctx_p->xmit_buf;
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      severe("rozofs_expgateway_resend_routing_common : not xmit_buf in routing context");
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** The system attempts first to forward the message toward load balancing group
    ** of an export gateway and then to the master export if the load balancing group
    ** of the export gateway is not available
    */
    lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
    if (lbg_id == -1)
    {
      errno = EPROTO;
      goto error;              
    }
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    rozofs_rpc_call_hdr_with_sz_t *com_hdr_p  = (rozofs_rpc_call_hdr_with_sz_t*) ruc_buf_getPayload(xmit_buf);  
    com_hdr_p->hdr.xid = ntohl(rozofs_tx_alloc_xid(rozofs_tx_ctx_p)); ; 
    /*
    ** store the receive call back and its associated parameter
    */
    if (recv_cbk != NULL) {
      rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    }
    rozofs_tx_ctx_p->user_param = fuse_buffer_ctx_p; 
    /*
    ** now send the message
    */
reloop:
    ret = north_lbg_send(lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       /*
       ** attempt to get the next available load balancing group
       */
       lbg_id = expgw_routing_get_next(routing_ctx_p,xmit_buf);
       if (lbg_id >= 0) goto reloop;
       
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);
    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p, ROZOFS_TMR_GET(TMR_EXPORT_PROGRAM));
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
    return -1;    
}



/**
* API for creation a transaction towards an storcli process

 The reference of the north load balancing is extracted for the client structure
 fuse_ctx_p:
 That API needs the pointer to the current fuse context. That nformation will be
 saved in the transaction context as userParam. It is intended to be used later when
 the client gets the response from the server
 encoding function;
 For making that API generic, the caller is intended to provide the function that
 will encode the message in XDR format. The source message that is encoded is 
 supposed to be pointed by msg2encode_p.
 Since the service is non-blocking, the caller MUST provide the callback function 
 that will be used for decoding the message
 

 @param clt        : pointer to the client structure
 @param timeout_sec : transaction timeout
 @param prog       : program
 @param vers       : program version
 @param opcode     : metadata opcode
 @param encode_fct : encoding function
 @msg2encode_p     : pointer to the message to encode
 @param recv_cbk   : receive callback function
 @param fuse_ctx_p : pointer to the fuse context
 @param storcli_idx      : identifier of the storcli
 @param fid: file identifier: needed for the storcli load balancing context
 
 @retval 0 on success;
 @retval -1 on error,, errno contains the cause
 */

int rozofs_storcli_send_common(exportclt_t * clt,uint32_t timeout_sec,uint32_t prog,uint32_t vers,
                              int opcode,xdrproc_t encode_fct,void *msg2encode_p,
                              sys_recv_pf_t recv_cbk,void *fuse_ctx_p,
			                  int storcli_idx,fid_t fid) 			       
{
    DEBUG_FUNCTION;
   
    uint8_t           *arg_p;
    uint32_t          *header_size_p;
    rozofs_tx_ctx_t   *rozofs_tx_ctx_p = NULL;
    void              *xmit_buf = NULL;
    int               bufsize;
    int               ret;
    int               position;
    XDR               xdrs;    
	struct rpc_msg   call_msg;
    uint32_t         null_val = 0;
    int              lbg_id;

    /*
    ** allocate a transaction context
    */
    rozofs_tx_ctx_p = rozofs_tx_alloc();  
    if (rozofs_tx_ctx_p == NULL) 
    {
       /*
       ** out of context
       ** --> put a pending list for the future to avoid repluing ENOMEM
       */
       TX_STATS(ROZOFS_TX_NO_CTX_ERROR);
       errno = ENOMEM;
       goto error;
    } 
    /*
    ** insert the storcli load balancing context in the  stclbg_hash_table hash table.
    ** the context is embedded in the transaction context  
    */
    stclbg_hash_table_insert_ctx(&rozofs_tx_ctx_p->rw_lbg,fid,storcli_idx);
    /*
    ** Get the load balancing group reference associated with the storcli
    */
    lbg_id = storcli_lbg_get_load_balancing_reference(storcli_idx);
    /*
    ** allocate an xmit buffer
    */  
    xmit_buf = ruc_buf_getBuffer(ROZOFS_TX_LARGE_TX_POOL);
    if (xmit_buf == NULL)
    {
      /*
      ** something rotten here, we exit we an error
      ** without activating the FSM
      */
      TX_STATS(ROZOFS_TX_NO_BUFFER_ERROR);
      errno = ENOMEM;
      goto error;
    } 
    /*
    ** store the reference of the xmit buffer in the transaction context: might be useful
    ** in case we want to remove it from a transmit list of the underlying network stacks
    */
    rozofs_tx_save_xmitBuf(rozofs_tx_ctx_p,xmit_buf);
    /*
    ** get the pointer to the payload of the buffer
    */
    header_size_p  = (uint32_t*) ruc_buf_getPayload(xmit_buf);
    arg_p = (uint8_t*)(header_size_p+1);  
    /*
    ** create the xdr_mem structure for encoding the message
    */
    bufsize = ruc_buf_getMaxPayloadLen(xmit_buf);
    xdrmem_create(&xdrs,(char*)arg_p,bufsize,XDR_ENCODE);
    /*
    ** fill in the rpc header
    */
    call_msg.rm_direction = CALL;
    /*
    ** allocate a xid for the transaction 
    */
	call_msg.rm_xid             = rozofs_tx_alloc_xid(rozofs_tx_ctx_p); 
    /*
    ** check the case of the READ since, we must set the value of the xid
    ** at the top of the buffer
    */
#if 1
    if ((opcode == STORCLI_READ)||(opcode == STORCLI_WRITE))
    {
       uint32_t *share_p;
       void *shared_buf_ref;

        RESTORE_FUSE_PARAM(fuse_ctx_p,shared_buf_ref);
        if (shared_buf_ref != NULL)
        {
           share_p = (uint32_t*)ruc_buf_getPayload(shared_buf_ref);
          *share_p = (uint32_t)call_msg.rm_xid;
	  /**
	  * copy the buffer for the case of the write
	  */
	  if (opcode == STORCLI_WRITE)
	  {
	     storcli_write_arg_t  *wr_args = (storcli_write_arg_t*)msg2encode_p;
	     /*
	     ** get the length to copy from the sshared memory
	     */
	     int len = share_p[1];
	     memcpy(&share_p[2],wr_args->data.data_val,len);	  
	  }
        }
    }
#endif
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	/* XXX: prog and vers have been long historically :-( */
	call_msg.rm_call.cb_prog = (uint32_t)prog;
	call_msg.rm_call.cb_vers = (uint32_t)vers;
	if (! xdr_callhdr(&xdrs, &call_msg))
    {
       /*
       ** THIS MUST NOT HAPPEN
       */
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;	
    }
    /*
    ** insert the procedure number, NULL credential and verifier
    */
    XDR_PUTINT32(&xdrs, (int32_t *)&opcode);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
    XDR_PUTINT32(&xdrs, (int32_t *)&null_val);
        
    /*
    ** ok now call the procedure to encode the message
    */
    if ((*encode_fct)(&xdrs,msg2encode_p) == FALSE)
    {
       TX_STATS(ROZOFS_TX_ENCODING_ERROR);
       errno = EPROTO;
       goto error;
    }
    /*
    ** Now get the current length and fill the header of the message
    */
    position = XDR_GETPOS(&xdrs);
    /*
    ** update the length of the message : must be in network order
    */
    *header_size_p = htonl(0x80000000 | position);
    /*
    ** set the payload length in the xmit buffer
    */
    int total_len = sizeof(*header_size_p)+ position;
    ruc_buf_setPayloadLen(xmit_buf,total_len);
    /*
    ** store the receive call back and its associated parameter
    */
    rozofs_tx_ctx_p->recv_cbk   = recv_cbk;
    rozofs_tx_ctx_p->user_param = fuse_ctx_p;    
    /*
    ** now send the message
    */
//    int lbg_id = storcli_lbg_get_load_balancing_reference();
    ret = north_lbg_send(lbg_id,xmit_buf);
    if (ret < 0)
    {
       TX_STATS(ROZOFS_TX_SEND_ERROR);
       errno = EFAULT;
      goto error;  
    }
    TX_STATS(ROZOFS_TX_SEND);

    /*
    ** OK, so now finish by starting the guard timer
    */
    rozofs_tx_start_timer(rozofs_tx_ctx_p,timeout_sec);  
//    if (*tx_ptr != NULL) *tx_ptr = rozofs_tx_ctx_p;
    return 0;  
    
  error:
    if (rozofs_tx_ctx_p != NULL) rozofs_tx_free_from_ptr(rozofs_tx_ctx_p);
//    if (xmit_buf != NULL) ruc_buf_freeBuffer(xmit_buf);    
    return -1;    
}
