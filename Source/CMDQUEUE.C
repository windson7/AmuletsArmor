/*-------------------------------------------------------------------------*
 * File:  CMDQUEUE.C
 *-------------------------------------------------------------------------*/
/**
 * The CmdQueue (or "Command Queue") is used to store up requests from
 * the client to the server.  As the commands are sent, an ACK packet
 * must be received before the next one is processed.  BUT, this isn't a
 * linear list of commands, but a list per packet type allowing most
 * commands to be handled in a mostly parallel format.  Most of this is
 * code has been replaced by the CSYNCPCK synchronized communication code
 * and the rest is incorrectly implemented for a peer to peer network
 * instead of a client/server network.
 * MORE WORK GOES HERE!
 *
 * @addtogroup CMDQUEUE
 * @brief Queue of Packets and Commands for Networking
 * @see http://www.amuletsandarmor.com/AALicense.txt
 * @{
 *
 *<!-----------------------------------------------------------------------*/
#include "COMM.H"
#include "CMDQUEUE.H"
#include "GENERAL.H"
#include "MEMORY.H"
#include "TICKER.H"

#ifdef COMPILE_OPTION_CREATE_PACKET_DATA_FILE
FILE *G_packetFile ;
#endif

typedef enum {
    PACKET_COMMAND_TYPE_LOSSLESS,
    PACKET_COMMAND_TYPE_LOSSFUL
} E_packetCommandType ;

typedef T_cmdQActionRoutine *T_comQActionList ;

typedef struct T_cmdQPacketStructTag {
    struct T_cmdQPacketStructTag *prev ;
    struct T_cmdQPacketStructTag *next ;
    T_word32 timeToRetry ;
    T_word32 extraData ;
    T_word16 retryTime ;
    T_cmdQPacketCallback p_callback ;
    T_packetLong packet ;
#ifndef NDEBUG
    T_byte8 tag[4] ;
#endif
} T_cmdQPacketStruct ;

typedef struct {
    T_cmdQPacketStruct *first ;
    T_cmdQPacketStruct *last ;
} T_cmdQStruct ;

static E_Boolean G_init = FALSE ;

static T_word16 G_activePort = 0xFFFF ;

static T_cmdQStruct *G_activeCmdQList = NULL ;

/** All callbacks are now set dynamically. **/
static T_cmdQActionRoutine G_cmdQActionList[PACKET_COMMAND_MAX] = {
    NULL,                  /* 0 ACK */
    NULL,                  /* 1 LOGIN */
    NULL,                  /* 2 RETRANSMIT */
    NULL,                  /* 3 TOWN_UI_MESSAGE */
    NULL,                  /* 4 PLAYER_ID_SELF */
    NULL,                  /* 5 REQUEST_PLAYER_ID */
    NULL,                  /* 6 GAME_REQUEST_JOIN */
    NULL,                  /* 7 GAME_RESPOND_JOIN */
    NULL,                  /* 8 GAME_START */
    NULL,                  /* 9 SYNC */
    NULL,                  /* 10 MESSAGE */
} ;

static E_packetCommandType G_CmdQTypeCommand[PACKET_COMMAND_MAX] = {
    PACKET_COMMAND_TYPE_LOSSFUL,                /* 0 ACK */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 1 LOGIN */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 2 RETRANSMIT */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 4 TOWN_UI_MESSAGE */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 5 PLAYER_ID_SELF */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 6 REQUEST_PLAYER_ID */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 7 GAME_REQUEST_JOIN */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 8 GAME START */
    PACKET_COMMAND_TYPE_LOSSFUL,                /* 9 SYNC */
    PACKET_COMMAND_TYPE_LOSSLESS,               /* 10 MESSAGE */
} ;

static T_cmdQStruct G_cmdQueues[MAX_COMM_PORTS][PACKET_COMMAND_MAX];

typedef struct {
    T_word32 time ;
    T_word32 throughput ;
    T_byte8 commandPos ;
} T_cmdQueueInfo ;

static T_cmdQueueInfo G_cmdQueueInfo[MAX_COMM_PORTS];

static T_cmdQueueInfo *P_activeQueueInfo = NULL ;

/** CMDQUEUE now controls the packet ID's. **/
static T_word32 G_nextPacketId = 0;

/* Internal prototypes: */
T_void ICmdQUpdateSendForPort(T_void) ;

T_void ICmdQDiscardPacket(T_byte8 commandNum) ;

T_void ICmdQSendPacket(
            T_packetEitherShortOrLong *p_packet,
            T_word16 retryTime,
            T_word32 extraData,
            T_cmdQPacketCallback p_callback) ;

static T_void ICmdQClearPort(T_void) ;

#ifndef NDEBUG
T_word32 G_packetsAlloc = 0 ;
T_word32 G_packetsFree = 0 ;
#endif

/*-------------------------------------------------------------------------*
 * Routine:  CmdQInitialize
 *-------------------------------------------------------------------------*/
/**
 *  CmdQInitialize clears out all the variables that are necessary for
 *  the CmdQ module.
 *
 *  NOTE: 
 *  This routine needs to be called AFTER all ports are opened.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQInitialize(T_void)
{
    DebugRoutine("CmdQInitialize") ;
    DebugCheck(G_init == FALSE) ;

    /* Note that we are now initialized. */
    G_init = TRUE ;

#ifdef COMPILE_OPTION_CREATE_PACKET_DATA_FILE
    G_packetFile = fopen("packets.dat", "w") ;
#endif

    /* Clear some of those global variables. */
    memset(G_cmdQueues, 0, sizeof(G_cmdQueues)) ;
    memset(G_cmdQueueInfo, 0, sizeof (G_cmdQueueInfo)) ;

#if 0
    /* Let's see how many ports there are. */
    numPorts = CommGetNumberPorts() ;

    /* Go through to each port and set it up. */
    for (port=0; port<numPorts; port++)  {
        /* Set the active port. */
        CommSetActivePortN(port) ;
    }
#endif

    /* Make the active port an illegal port. */
    G_activePort = 0xFFFF ;

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQRegisterClientCallbacks
 *-------------------------------------------------------------------------*/
/**
 *  These functions each take a pointer to an array of function pointers,
 *  with PACKET_COMMAND_MAX entries.  ..ServerCallbacks sets this pointer
 *  as the list of server packet callbacks, and ..ClientCallbacks sets this
 *  as the list of client packet callbacks.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQRegisterClientCallbacks (T_cmdQActionRoutine *callbacks)
{
   T_word16 i;

   DebugRoutine ("CmdQRegisterClientCallbacks");

   for (i=0; i < PACKET_COMMAND_MAX; i++)
      G_cmdQActionList[i] = callbacks[i];

   DebugEnd ();
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQFinish
 *-------------------------------------------------------------------------*/
/**
 *  CmdQFinish unallocates any memory or structures that are no longer
 *  need by the CmdQ module.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQFinish(T_void)
{
    DebugRoutine("CmdQFinish") ;
    DebugCheck(G_init == TRUE) ;

    /* Note that we are now finished. */
    G_init = FALSE ;

#ifdef COMPILE_OPTION_CREATE_PACKET_DATA_FILE
    fclose(G_packetFile) ;
#endif

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQSetActivePortNum
 *-------------------------------------------------------------------------*/
/**
 *  CmdQSetActivePortNum sets up the Cmd Queue module for the given
 *  port (and makes the given port the active communications port).
 *
 *  @param num -- Number of port to make active.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQSetActivePortNum(T_word16 num)
{
    DebugRoutine("CmdQSetActivePortNum") ;
    DebugCheck(G_init == TRUE) ;

    /* Go ahead and set the active port. */
//    CommSetActivePortN(num) ;
    G_activePort = num ;

    /* Declare which list of command queues we want active (for this port). */
    G_activeCmdQList = &G_cmdQueues[num][0] ;

    /* Set a poiner to that queue's overall information. */
    P_activeQueueInfo = &G_cmdQueueInfo[num] ;

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQGetActivePortNum
 *-------------------------------------------------------------------------*/
/**
 *  CmdQGetActivePortNum returns the number of the active port being
 *  used.
 *
 *  @return Number of port.
 *
 *<!-----------------------------------------------------------------------*/
T_word16 CmdQGetActivePortNum(T_void)
{
    T_word16 portNum ;

    DebugRoutine("CmdQGetActivePortNum") ;
    DebugCheck(G_init == TRUE) ;

    /* Just get it. */
    portNum = G_activePort ;

    DebugEnd() ;

    return portNum ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQSendShortPacket
 *-------------------------------------------------------------------------*/
/**
 *  CmdQSendShortPacket prepares a short packet for sending and sends
 *  it over to ICmdQSendPacket for processing.
 *
 *  @param p_packet -- Packet to send
 *  @param retryTime -- How long to wait for acknowledgement
 *      before trying to send again.
 *  @param extraData -- Extra data for the callback routine.
 *  @param p_callback -- Routine to be called when either
 *      the packet is lost or sent.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQSendShortPacket(
            T_packetShort *p_packet,
            T_word16 retryTime,
            T_word32 extraData,
            T_cmdQPacketCallback p_callback)
{
    DebugRoutine("CmdQSendShortPacket") ;
    DebugCheck(p_packet != NULL) ;

    p_packet->header.packetLength = SHORT_PACKET_LENGTH ;
    ICmdQSendPacket(
        (T_packetEitherShortOrLong *)p_packet,
        retryTime,
        extraData,
        p_callback) ;

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQSendLongPacket
 *-------------------------------------------------------------------------*/
/**
 *  CmdQSendLongPacket  prepares a long  packet for sending and sends
 *  it over to ICmdQSendPacket for processing.
 *
 *  @param p_packet -- Packet to send
 *  @param retryTime -- How long to wait for acknowledgement
 *      before trying to send again.
 *  @param extraData -- Extra data for the callback routine.
 *  @param p_callback -- Routine to be called when either
 *      the packet is lost or sent.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQSendLongPacket(
            T_packetLong *p_packet,
            T_word16 retryTime,
            T_word32 extraData,
            T_cmdQPacketCallback p_callback)
{
    DebugRoutine("CmdQSendLongPacket") ;
    DebugCheck(p_packet != NULL) ;

    p_packet->header.packetLength = LONG_PACKET_LENGTH ;
    ICmdQSendPacket(
        (T_packetEitherShortOrLong *)p_packet,
        retryTime,
        extraData,
        p_callback) ;

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  ICmdQSendPacket
 *-------------------------------------------------------------------------*/
/**
 *  ICmdQSendPacket does all the work of placing a packet in one of the
 *  active command queues and prepares it for sending out.
 *
 *  @param p_packet -- Packet to send
 *  @param retryTime -- How long to wait for acknowledgement
 *      before trying to send again.
 *  @param extraData -- Extra data for the callback routine.
 *  @param p_callback -- Routine to be called when either
 *      the packet is lost or sent.
 *
 *<!-----------------------------------------------------------------------*/
T_void ICmdQSendPacket(
            T_packetEitherShortOrLong *p_packet,
            T_word16 retryTime,
            T_word32 extraData,
            T_cmdQPacketCallback p_callback)
{
    T_byte8 command ;
    T_cmdQPacketStruct *p_cmdPacket ;
    DebugRoutine("ICmdQSendPacket") ;

    DebugCheck (G_init == TRUE);

    /* All we have to do is put the packet in the queue's list of packets. */
    /* That's all.  We'll let CmdQUpdateAllSends do the actual processing. */

    /* First, what command are we sending? */
    command = p_packet->data[0] ;
    DebugCheck(command < PACKET_COMMAND_UNKNOWN) ;

    /** Give the packet a unique packet ID **/
    PacketSetId (
        p_packet,
        G_nextPacketId++);

    /* Allocate a structure to hold the information plus more. */
    p_cmdPacket = MemAlloc(sizeof(T_cmdQPacketStruct)) ;

#ifndef NDEBUG
    G_packetsAlloc++ ;
#endif

    /* clear out this packet. */
    memset(p_cmdPacket, 0, sizeof(T_cmdQPacketStruct)) ;
#ifndef NDEBUG
    strcpy(p_cmdPacket->tag, "CmQ") ;
#endif

    /* Copy in the packet data. */
    p_cmdPacket->packet = *((T_packetLong *)p_packet) ;

    /* Now attack this new packet to the appropriate command list. */
    p_cmdPacket->next = G_activeCmdQList[command].first ;

    /* Update the first and last links. */
    if (G_activeCmdQList[command].first != NULL)  {
        G_activeCmdQList[command].first->prev = p_cmdPacket ;
    } else {
        G_activeCmdQList[command].last = p_cmdPacket ;
    }

    /* Declare this as the new first. */
    G_activeCmdQList[command].first = p_cmdPacket ;

    /* Fill in given data. */
    p_cmdPacket->extraData = extraData ;
    p_cmdPacket->p_callback = p_callback ;
    p_cmdPacket->retryTime = retryTime ;

    /* Put in a time of zero to denote first sending. */
    p_cmdPacket->timeToRetry = 0 ;

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQUpdateAllSends
 *-------------------------------------------------------------------------*/
/**
 *  CmdQUpdateAllSends goes through all the ports and determines what
 *  data needs to be sent and what should wait.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQUpdateAllSends(T_void)
{
    DebugRoutine("CmdQUpdateAllSends") ;

#if 0
    /* Let's see how many ports there are. */
    numPorts = CommGetNumberPorts() ;

    /* Go through to each port looking for incoming data. */
    for (port=0; port<numPorts; port++)  {
        /* Set the active port (and any additional information). */
        CmdQSetActivePortNum(port) ;

        ICmdQUpdateSendForPort() ;
    }
#else
    CmdQSetActivePortNum(0) ;
    ICmdQUpdateSendForPort() ;
#endif

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQUpdateAllReceives
 *-------------------------------------------------------------------------*/
/**
 *  CmdQUpdateAllReceives goes through all the ports looking for data
 *  to take in.  All data that is received is then processed and the
 *  appropriate action for the command is called.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQUpdateAllReceives(T_void)
{
    T_packetLong packet ;
    T_sword16 status ;
    T_byte8 command ;
    T_byte8 ackCommand ;
    T_word32 packetId ;
    T_packetShort ackPacket ;
    E_Boolean packetOkay;
    T_word16 port ;
    T_word16 numPorts ;

    DebugRoutine("CmdQUpdateAllReceives") ;
    INDICATOR_LIGHT(260, INDICATOR_GREEN) ;
    DebugCheck(G_init == TRUE) ;

    /* Let's see how many ports there are. */
    DebugCheckValidStack() ;
//    numPorts = CommGetNumberPorts() ;
    numPorts = 1 ;
    DebugCheckValidStack() ;

    /* Go through to each port looking for incoming data. */
    for (port=0; port<numPorts; port++)  {
        /* Set the active port (and any additional information). */
        DebugCheckValidStack() ;

        /* Hunting for a bug... */
        /* ... . */

        CmdQSetActivePortNum(port) ;
        DebugCheckValidStack() ;

        /* Loop while there are packets to get. */
        do {
            DebugCompare("CmdQUpdateAllReceives") ;
            DebugCheckValidStack() ;
            /* Try getting a packet. */
            status = PacketGet(&packet) ;
            DebugCheckValidStack() ;

            /* Did we get a packet? */
            if (status == 0)  {
                /* Yes, we did.  See what command is being issued. */
                command = packet.data[0] ;

                /* Make sure it is a legal commands.  Unfortunately, */
                /* we'll have to ignore those illegal commands. */
                if (command < PACKET_COMMAND_UNKNOWN)  {
                    /* Is it an ACK packet? */


//printf("R(%d) %2d %ld %ld\n", CmdQGetActivePortNum(), packet.data[0], packet.header.id, TickerGet()) ;  fflush(stdout) ;
#ifdef COMPILE_OPTION_CREATE_PACKET_DATA_FILE
fprintf(G_packetFile, "R(%d) %2d %ld %ld\n", CmdQGetActivePortNum(), packet.data[0], packet.header.id, SyncTimeGet()) ; fflush(G_packetFile) ;
#endif

                    if (command == PACKET_COMMAND_ACK)  {
                        /* Yes, it is an ack.  See what command it is */
                        /* acknowledging. */
                        ackCommand = packet.data[1] ;

                        /* Is that a valid command? */
                        if (ackCommand < PACKET_COMMAND_UNKNOWN)  {
                            INDICATOR_LIGHT(264, INDICATOR_GREEN) ;
                            /* Yes.  But is it a lossless command? */
                            if (G_CmdQTypeCommand[ackCommand] ==
                                PACKET_COMMAND_TYPE_LOSSLESS)  {
                                /* Get the packet id. */
                                packetId = *((T_word32 *)(&(packet.data[2]))) ;
                                /* Is there a list at that point? */
                                if (G_activeCmdQList[ackCommand].last != NULL)  {
                                    /* Yes.  Is the ack for the same packet */
                                    /* waiting? */
                                    if (G_activeCmdQList[ackCommand].last->packet.header.id == packetId)  {
                                        /* Yes. We can now discard it. */
                                        DebugCheckValidStack() ;
                                        ICmdQDiscardPacket(ackCommand) ;
                                        DebugCheckValidStack() ;
                                    }
                                }
                            }
                            INDICATOR_LIGHT(264, INDICATOR_RED) ;
                        }
                    } else {
                        /* No, do the normal action. */

                        /** If this is a lossful packet, always call the **/
                        /** callback routine. **/
                        packetOkay = TRUE;

                        /* Is this a lossless command? */
                        if (G_CmdQTypeCommand[command] ==
                                PACKET_COMMAND_TYPE_LOSSLESS)  {
                            INDICATOR_LIGHT(268, INDICATOR_GREEN) ;
                            memset(&ackPacket, 0xFF, sizeof(ackPacket));
                            /* Yes, it is.  We need to send an ACK that */
                            /* we got it. */
                            /* Make an ack packet with the packet's */
                            /* command and id we received. */
                            ackPacket.data[0] = PACKET_COMMAND_ACK ;
                            ackPacket.data[1] = command ;
                            *((T_word32 *)(&ackPacket.data[2])) =
                                packet.header.id ;
                            /* Send it!  Note that we go through our */
                            /* routines. */
                            INDICATOR_LIGHT(272, INDICATOR_GREEN) ;
                            DebugCheckValidStack() ;
                            CmdQSendShortPacket(
                                &ackPacket,
                                140,  /* Once two seconds is plenty fast */
                                0,  /* No extra data since no callback. */
                                NULL) ;  /* No callback. */
                            INDICATOR_LIGHT(272, INDICATOR_RED) ;
                            DebugCheckValidStack() ;

                            INDICATOR_LIGHT(268, INDICATOR_RED) ;
                        }
                        /* IF the packet is a new packet or a lossful one, */
                        /* we'll go ahead and do the appropriate action */
                        /* on this side. */
//printf("Considering %p [%d]\n", G_cmdQActionList[command], command) ;
                        if ((G_cmdQActionList[command] != NULL) &&
                            (packetOkay == TRUE))  {
                            /* Call the appropriate action item. */
                            INDICATOR_LIGHT(276, INDICATOR_GREEN) ;
//printf("Did go: (%d) %d, %p\n", command, packetOkay, G_cmdQActionList[command]) ; fflush(stdout) ;
                            DebugCheckValidStack() ;
                            G_cmdQActionList[command]
                                   ((T_packetEitherShortOrLong *)&packet) ;
                            DebugCheckValidStack() ;
                            INDICATOR_LIGHT(276, INDICATOR_RED) ;
                            DebugCompare("CmdQUpdateAllReceives") ;
                        } else {
//printf("Didn't go: (%d) %d, %p\n", command, packetOkay, G_cmdQActionList[command]) ; fflush(stdout) ;
                        }
                    }
                }
            }
            DebugCheckValidStack() ;
        } while (status == 0) ;
    }
    DebugEnd() ;

    INDICATOR_LIGHT(260, INDICATOR_RED) ;
}

/*-------------------------------------------------------------------------*
 * Routine:  ICmdQUpdateSendForPort
 *-------------------------------------------------------------------------*/
/**
 *  ICmdQUpdateSendForPort updates the sending of all packets for this
 *  single port (the currently active port).  Packets that need to be
 *  transferred are sent.  Those that don't, don't.  The amount of packets
 *  sent depends on the baud rate and the last time this routine was
 *  called for this port.
 *
 *<!-----------------------------------------------------------------------*/
T_void ICmdQUpdateSendForPort(T_void)
{
    T_byte8 currentCmd;
    T_cmdQPacketStruct *p_packet;
    T_byte8 packetLength;
    T_word32 time;
    T_sword16 status;
    E_Boolean sentOne;
    T_word16 bytesused;
    T_word32 maxOutput;
    E_Boolean onceFlag = FALSE;
    E_Boolean sentAny;

    DebugRoutine("ICmdQUpdateSendForPort");

    /* Get the current time. */
    time = TickerGet();

    bytesused = 0;
    maxOutput = 100;
    currentCmd = 0;
    if (bytesused < maxOutput) {
        do {
            sentAny = FALSE;
            do {
                sentOne = FALSE;
                /* Try sending the command at currentCmd. */
                /* See if there is anything that needs to be sent. */
                while ((G_activeCmdQList[currentCmd].last != NULL)
                        && (bytesused < maxOutput)) {
                    DebugCheck(currentCmd < PACKET_COMMAND_UNKNOWN);
                    /* Yes, there appears to be a packet at the end of the list. */
                    /* Get a hold on that packet struct. */
                    p_packet = G_activeCmdQList[currentCmd].last;
#ifndef NDEBUG
                    if (strcmp(p_packet->tag, "CmQ") != 0) {
                        printf("Bad packet %p\n", p_packet);
                        DebugCheck(FALSE);
                    }
#endif

                    /* See if it is time to send it. */
                    /** (It's always time to send an ACK.) **/
                    if ((currentCmd == PACKET_COMMAND_ACK)
                            || (p_packet->timeToRetry < time)) {
                        /* Yes, it is time to send it. */
                        packetLength = p_packet->packet.header.packetLength;
                        /* Add the count to the output. */
                        bytesused += packetLength + sizeof(T_packetHeader);

                        /* Make sure this didn't go over the threshold. */
                        if (bytesused >= maxOutput)
                            break;

                        /* Let's send that packet. */
                        DirectTalkSetDestinationAll();  //TODO: ?? All??
                        status =
                                PacketSend(
                                        (T_packetEitherShortOrLong *)(&p_packet->packet));
                        sentAny = TRUE;

#ifdef COMPILE_OPTION_CREATE_PACKET_DATA_FILE
                        fprintf(G_packetFile, "S(%d) cmd=%2d, id=%ld, time=%ld\n", CmdQGetActivePortNum (), p_packet->packet.data[0], p_packet->packet.header.id, SyncTimeGet()); fflush(G_packetFile);
#endif
                        /* Was the packet actually sent? */
                        if (status == 0) {
                            /* Packet was sent correctly (as far as we know). */
                            /* Is this a lossful or lossless command queue? */
                            if (G_CmdQTypeCommand[currentCmd]
                                    == PACKET_COMMAND_TYPE_LOSSFUL) {
                                /* Lossful.  Means we can go ahead and */
                                /* discard this packet. */
                                ICmdQDiscardPacket(currentCmd);
                            } else {
                                /* Lossless.  Means we must wait for an ACK */
                                /* packet to confirm that we were sent. */
                                /* Until then, we can't discard the packet. */
                                /* But we might have to resend latter. */
                                /* Set up the retry time. */
                                p_packet->timeToRetry = time
                                        + p_packet->retryTime;
                            }

                            /* Note that we sent the packet (or at least) */
                            /* gave a good try. */
                            sentOne = TRUE;
                        } else {
                            /* Packet was NOT sent correctly. */
                            /** -- IFF the packet is lossless... **/
                            /* We'll have to retry later.  Change the time, */
                            /* but don't remove it from the linked list. */
                            if (G_CmdQTypeCommand[currentCmd]
                                    == PACKET_COMMAND_TYPE_LOSSLESS) {
                                p_packet->timeToRetry = time
                                        + p_packet->retryTime;
                            } else {
                                /* Waste it!  We can't wait around */
                                ICmdQDiscardPacket(currentCmd);
                            }
                        }
                    } else {
                        /* Don't loop if it is not time to send. */
                        break;
                    }

                    /** Now, if we aren't checking the ACK queue, we need **/
                    /** to break after the first send.  If we are, we **/
                    /** want to dump everything in the queue. **/
                    if (currentCmd != PACKET_COMMAND_ACK)
                        break;
                }

                /* Update to the next command. */
                currentCmd++;
            } while (currentCmd < PACKET_COMMAND_MAX);

        } while ((bytesused < maxOutput) && (sentAny == TRUE));
    }

    P_activeQueueInfo->commandPos = currentCmd;

    DebugEnd();
}

/*-------------------------------------------------------------------------*
 * Routine:  ICmdQDiscardPacket
 *-------------------------------------------------------------------------*/
/**
 *  ICmdQDiscardPacket removes a packet from the given command queue in
 *  the current command queue list.
 *
 *  @param commandNum -- Command in command list to do a
 *      packet discard on.
 *
 *<!-----------------------------------------------------------------------*/
T_void ICmdQDiscardPacket(T_byte8 commandNum)
{
    T_cmdQStruct *p_cmdQ ;
    T_cmdQPacketStruct *p_packet ;

    DebugRoutine("ICmdQDiscardPacket") ;
    DebugCheck(commandNum < PACKET_COMMAND_UNKNOWN) ;

    /* Get a quick pointer to the command's queue. */
    p_cmdQ = &G_activeCmdQList[commandNum] ;

    p_packet = p_cmdQ->last ;
    /* Make sure there is a last entry. */
    DebugCheck(p_packet != NULL) ;

    if (p_packet != NULL)  {
#ifndef NDEBUG
        /* Check for the tag. */
        if (strcmp(p_packet->tag, "CmQ") != 0)  {
            printf("Bad packet %p\n", p_packet) ;
            DebugCheck(FALSE) ;
        }
#endif
        DebugCheck(p_packet->prev != p_packet) ;
        p_cmdQ->last = p_packet->prev ;
        if (p_cmdQ->last == NULL)
            p_cmdQ->first = NULL ;
        else {
            p_cmdQ->last->next = NULL ;
        }

        /* Before we free it, we need to call the callback routine for */
        /* this packet to say, "Hey!  We're done with you." */
        /* Of course, we must check to see if there is a callback routine. */
        if (p_packet->p_callback != NULL)  {
            p_packet->p_callback(
                p_packet->extraData,
                (T_packetEitherShortOrLong *)&p_packet->packet) ;
        }


#ifndef NDEBUG
        /* Mark the packet as gone. */
        strcpy(p_packet->tag, "c_q") ;
#endif
        MemFree(p_packet) ;

#ifndef NDEBUG
        G_packetsFree++ ;
#endif
    }

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQSendPacket
 *-------------------------------------------------------------------------*/
/**
 *  CmdQSendPacket sets up a short or long packet for sending (and does
 *  so by calling CmdQSendShortPacket or CmdQSendLongPacket).
 *
 *  NOTE: 
 *  The packetLength field in the packet's header field MUST be set
 *  to either short or long.
 *
 *  @param p_packet -- Packet to send
 *  @param retryTime -- How long to wait for acknowledgement
 *      before trying to send again.
 *  @param extraData -- Extra data for the callback routine.
 *  @param p_callback -- Routine to be called when either
 *      the packet is lost or sent.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQSendPacket(
            T_packetEitherShortOrLong *p_packet,
            T_word16 retryTime,
            T_word32 extraData,
            T_cmdQPacketCallback p_callback)
{
    DebugRoutine("CmdQSendPacket") ;

    /* Looks like we had something to send. */
    ICmdQSendPacket(
        (T_packetEitherShortOrLong *)p_packet,
        retryTime,
        extraData,
        p_callback) ;

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQClearAllPorts
 *-------------------------------------------------------------------------*/
/**
 *  CmdQClearAllPorts goes through each port and clears out any packets
 *  that are waiting to be sent or are incoming.  ACK packets are left
 *  alone.
 *
 *  NOTE: 
 *  Make sure that this routine is only called when all communications
 *  are finalized.
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQClearAllPorts(T_void)
{
    T_word16 numPorts ;
    T_word16 port ;
    DebugRoutine("CmdQClearAllPorts") ;

    /* Let's see how many ports there are. */
//    numPorts = CommGetNumberPorts() ;
    numPorts = 1 ;

    /* Go through to each port and set it up. */
    for (port=0; port<numPorts; port++)  {
        /* Set the active port. */
        CmdQSetActivePortNum(port) ;

        ICmdQClearPort() ;
    }

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  ICmdQClearPort
 *-------------------------------------------------------------------------*/
/**
 *  ICmdQClearPort removes all outgoing packets for the current port
 *  and all incoming packets, too.  ACK packets are left alone.
 *
 *  NOTE: 
 *  Make sure that this routine is only called when all communications
 *  are finalized.
 *
 *<!-----------------------------------------------------------------------*/
static T_void ICmdQClearPort(T_void)
{
    T_word16 i ;
    T_cmdQStruct *p_cmdQ ;
    T_cmdQPacketStruct *p_packet ;
    T_packetLong packet ;

    DebugRoutine("ICmdQClearPort") ;

    /* First, read in all the incoming packets (and ignore them). */
    while (PacketGet(&packet) == 0)
        { }

    /* Catch all the outgoing packets. */
    /* Go through all the queues looking for packets to remove. */
    for (i=1; i<PACKET_COMMAND_MAX; i++)  {
        for (;;)  {
            p_cmdQ = &G_activeCmdQList[i] ;
            p_packet = p_cmdQ->last ;
            if (p_packet != NULL)  {
#ifndef NDEBUG
                /* Check for the tag. */
                if (strcmp(p_packet->tag, "CmQ") != 0)  {
                    printf("Bad packet %p\n", p_packet) ;
                    DebugCheck(FALSE) ;
                }
#endif
                /* Found a packet, remove it. */
                p_cmdQ->last = p_packet->prev ;
                if (p_cmdQ->last == NULL)
                    p_cmdQ->first = NULL ;
                else {
                    p_cmdQ->last->next = NULL ;
                }

#ifndef NDEBUG
                /* Mark the packet as gone. */
                strcpy(p_packet->tag, "c_q") ;
                G_packetsFree++ ;
#endif

                /* Delete it. */
                MemFree(p_packet) ;
            } else {
                /* No packet.  Stop looping on this queue. */
                break ;
            }
        }
    }

    DebugEnd() ;
}

/*-------------------------------------------------------------------------*
 * Routine:  CmdQForcedReceive
 *-------------------------------------------------------------------------*/
/**
 *  CmdQForcedReceive makes the command queue act like it just received
 *  the packet from the inport.  All processing is the same.
 *
 *  NOTE: 
 *  NEVER PASS AN ACK PACKET TO THIS ROUTINE.  It does not properly
 *  work with them.
 *  Be sure to make a call to CmdQSetActivePortNum or else this routine
 *  will be unpredictable.
 *
 *  @param p_packet -- Packet being forced in
 *
 *<!-----------------------------------------------------------------------*/
T_void CmdQForcedReceive(T_packetEitherShortOrLong *p_packet)
{
    T_word16 command ;

    DebugRoutine("CmdQForcedReceive") ;

    command = p_packet->data[0] ;

    if (G_cmdQActionList[command] != NULL)  {
        /* Call the appropriate action item. */
        command = p_packet->data[0] ;
        G_cmdQActionList[command](p_packet) ;
        DebugCompare("CmdQForcedReceive") ;
    }

    DebugEnd() ;
}

/** @} */
/*-------------------------------------------------------------------------*
 * End of File:  CMDQUEUE.C
 *-------------------------------------------------------------------------*/