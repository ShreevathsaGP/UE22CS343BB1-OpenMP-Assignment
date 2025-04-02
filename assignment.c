#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define NUM_PROCS 4
#define MEM_SIZE 16
#define CACHE_SIZE 4
#define MAX_INSTR_NUM 32
#define MSG_BUFFER_SIZE 256

typedef unsigned char byte;

typedef enum { MODIFIED, EXCLUSIVE, SHARED, INVALID } cacheLineState;

typedef enum { EM, S, U } directoryEntryState;

typedef enum { 
    READ_REQUEST,
    WRITE_REQUEST,
    REPLY_RD,
    REPLY_WR,
    REPLY_ID,
    INV,
    UPGRADE,
    WRITEBACK_INV,
    WRITEBACK_INT,
    FLUSH,
    FLUSH_INVACK,
    EVICT_SHARED,
    EVICT_MODIFIED
} transactionType;

typedef struct instruction {
    byte type;
    byte address;
    byte value;
} instruction;

typedef struct cacheLine {
    byte address;
    byte value;
    cacheLineState state;
} cacheLine;

typedef struct directoryEntry {
    byte bitVector;
    directoryEntryState state;
} directoryEntry;

typedef struct message {
    transactionType type;
    int sender;
    byte address;
    byte value;
    byte bitVector;
    int secondReceiver;
    directoryEntryState dirState;
} message;

typedef struct messageBuffer {
    message queue[ MSG_BUFFER_SIZE ];
    int head;
    int tail;
    int count;
} messageBuffer;

typedef struct processorNode {
    cacheLine cache[ CACHE_SIZE ];
    byte memory[ MEM_SIZE ];
    directoryEntry directory[ MEM_SIZE ];
    instruction instructions[ MAX_INSTR_NUM ];
    int instructionCount;
} processorNode;

void initializeProcessor( int thread_id, processorNode *node, char *dir_name );
void sendMessage( int receiver, message msg );
void handleCacheReplacement( int sender, cacheLine old_cache_line );
void printProcessorState( int processor_id, processorNode node );

messageBuffer message_buffers[ NUM_PROCS ];
omp_lock_t msg_buffer_locks[ NUM_PROCS ];

int main( int argc, char * argv[] ) {
    if (argc < 2) {
        fprintf( stderr, "Usage: %s <test_directory>\n", argv[0] );
        return EXIT_FAILURE;
    }
    char *input_dir = argv[1];
    
    omp_set_num_threads(NUM_PROCS);

    for ( int idx = 0; idx < NUM_PROCS; idx++ ) {
        message_buffers[ idx ].count = 0;
        message_buffers[ idx ].head = 0;
        message_buffers[ idx ].tail = 0;
        omp_init_lock( &msg_buffer_locks[ idx ] );
    }
    processorNode local_node;

    #pragma omp parallel private(local_node)
    {
        int current_thread = omp_get_thread_num();
        initializeProcessor( current_thread, &local_node, input_dir );
        #pragma omp barrier

        message incoming_msg;
        message response_msg;
        instruction current_instr;
        int instr_counter = -1;
        int should_print = 1;
        byte awaiting_response = 0;

        while ( true ) {
            while ( 
                message_buffers[ current_thread ].count > 0 &&
                message_buffers[ current_thread ].head != message_buffers[ current_thread ].tail
            ) {
                if ( should_print == 0 ) {
                    should_print++;
                }
                int queue_head = message_buffers[ current_thread ].head;
                incoming_msg = message_buffers[ current_thread ].queue[ queue_head ];
                message_buffers[ current_thread ].head = ( queue_head + 1 ) % MSG_BUFFER_SIZE;
                message_buffers[ current_thread ].count--;

                byte target_node = (incoming_msg.address >> 4) & 0x0F;
                byte mem_location = incoming_msg.address & 0x0F;
                byte cache_slot = mem_location % CACHE_SIZE;

                switch ( incoming_msg.type ) {
                    case READ_REQUEST:
                        if (local_node.directory[mem_location].state == EM) {
                            response_msg = (message) {
                                .type = WRITEBACK_INT,
                                .sender = current_thread,
                                .address = incoming_msg.address,
                                .secondReceiver = incoming_msg.sender,
                            };

                            int previous_owner = __builtin_ctz(local_node.directory[mem_location].bitVector);
                            sendMessage( previous_owner, response_msg );
                        } else if (local_node.directory[mem_location].state == S) {
                            response_msg = (message) {
                                .type = REPLY_RD,
                                .sender = current_thread,
                                .address = incoming_msg.address,
                                .value = local_node.memory[ mem_location ],
                                .dirState = S
                            };
                            sendMessage( incoming_msg.sender, response_msg );
                            local_node.directory[ mem_location ].bitVector |= (1 << incoming_msg.sender);
                        } else if (local_node.directory[mem_location].state == U) {
                            response_msg = (message) {
                                .type = REPLY_RD,
                                .sender = current_thread,
                                .address = incoming_msg.address,
                                .value = local_node.memory[ mem_location ],
                                .dirState = EM
                            };
                            sendMessage( incoming_msg.sender, response_msg );
                            local_node.directory[ mem_location ].state = EM;
                            local_node.directory[ mem_location ].bitVector = (1 << incoming_msg.sender);
                        }
                        break;

                    case REPLY_RD:
                        if (local_node.cache[cache_slot].address != incoming_msg.address &&
                            local_node.cache[cache_slot].state != INVALID) {
                            handleCacheReplacement( current_thread, local_node.cache[ cache_slot ] );
                        }
                        local_node.cache[ cache_slot ].address = incoming_msg.address;
                        local_node.cache[ cache_slot ].value = incoming_msg.value;
                        local_node.cache[ cache_slot ].state = (incoming_msg.dirState == S) ? SHARED : EXCLUSIVE;
                        awaiting_response = 0;
                        break;

                    case WRITEBACK_INT:
                        response_msg = (message) {
                            .type = FLUSH,
                            .sender = current_thread,
                            .address = incoming_msg.address,
                            .value = local_node.cache[ cache_slot ].value,
                            .secondReceiver = incoming_msg.secondReceiver,
                        };
                        sendMessage( target_node, response_msg );

                        if (target_node != incoming_msg.secondReceiver)
                            sendMessage( incoming_msg.secondReceiver, response_msg );

                        local_node.cache[ cache_slot ].state = SHARED;
                        break;

                    case FLUSH:
                        if (current_thread == target_node) {
                            local_node.directory[mem_location].state = S;
                            local_node.directory[mem_location].bitVector |= (1 << incoming_msg.secondReceiver);
                            local_node.memory[mem_location] = incoming_msg.value;
                        }

                        if (current_thread == incoming_msg.secondReceiver) {
                            if (local_node.cache[ cache_slot ].address != incoming_msg.address &&
                                local_node.cache[ cache_slot ].state != INVALID) {
                                handleCacheReplacement( current_thread, local_node.cache[ cache_slot ] );
                            }
                            local_node.cache[ cache_slot ].address = incoming_msg.address;
                            local_node.cache[ cache_slot ].value = incoming_msg.value;
                            local_node.cache[ cache_slot ].state = SHARED;
                        }

                        awaiting_response = 0;
                        break;

                    case UPGRADE:
                        byte other_nodes = local_node.directory[mem_location].bitVector & (~(1 << incoming_msg.sender));

                        response_msg = (message) {
                            .type = REPLY_ID,
                            .sender = current_thread,
                            .address = incoming_msg.address,
                            .bitVector = other_nodes,
                        };
                        sendMessage( incoming_msg.sender, response_msg );

                        local_node.directory[mem_location].state = EM;
                        local_node.directory[mem_location].bitVector = (1 << incoming_msg.sender);

                        break;

                    case REPLY_ID:
                        for (int i = 0; i < NUM_PROCS; i++) {
                            if (incoming_msg.bitVector & (1 << i)) {
                                response_msg = (message) {
                                    .type = INV,
                                    .sender = current_thread,
                                    .address = incoming_msg.address,
                                };
                                sendMessage( i, response_msg );
                            }
                        }

                        if (local_node.cache[ cache_slot ].address != incoming_msg.address &&
                            local_node.cache[ cache_slot ].state != INVALID) {
                            handleCacheReplacement( current_thread, local_node.cache[ cache_slot ] );
                        }

                        local_node.cache[ cache_slot ].address = incoming_msg.address;
                        local_node.cache[ cache_slot ].value = current_instr.value;
                        local_node.cache[ cache_slot ].state = MODIFIED;

                        awaiting_response = 0;
                        break;

                    case INV:
                        if (local_node.cache[ cache_slot ].address == incoming_msg.address) {
                            local_node.cache[ cache_slot ].state = INVALID;
                        }
                        break;

                    case WRITE_REQUEST:
                        if (local_node.directory[mem_location].state == U) {
                            response_msg = (message) {
                                .type = REPLY_WR,
                                .sender = current_thread,
                                .address = incoming_msg.address,
                            };

                            sendMessage( incoming_msg.sender, response_msg );
                        } else if (local_node.directory[mem_location].state == S) {
                            byte other_nodes = local_node.directory[mem_location].bitVector & (~(1 << incoming_msg.sender));
                            response_msg = (message) {
                                .type = REPLY_ID,
                                .sender = current_thread,
                                .address = incoming_msg.address,
                                .bitVector = other_nodes,
                            };
                            sendMessage( incoming_msg.sender, response_msg );
                        } else if (local_node.directory[mem_location].state == EM) {
                            response_msg = (message) {
                                .type = WRITEBACK_INV,
                                .sender = current_thread,
                                .address = incoming_msg.address,
                                .value = incoming_msg.value,
                                .secondReceiver = incoming_msg.sender,
                            };

                            int previous_owner = __builtin_ctz(local_node.directory[mem_location].bitVector);
                            sendMessage( previous_owner, response_msg );
                        }

                        local_node.directory[mem_location].state = EM;
                        local_node.directory[mem_location].bitVector = (1 << incoming_msg.sender);

                        break;

                    case REPLY_WR:
                        handleCacheReplacement( current_thread, local_node.cache[ cache_slot ] );

                        local_node.cache[ cache_slot ].address = incoming_msg.address;
                        local_node.cache[ cache_slot ].value = current_instr.value;
                        local_node.cache[ cache_slot ].state = MODIFIED;

                        awaiting_response = 0;
                        break;

                    case WRITEBACK_INV:
                        response_msg = (message) {
                            .type = FLUSH_INVACK,
                            .sender = current_thread,
                            .address = incoming_msg.address,
                            .value = local_node.cache[ cache_slot ].value,
                            .secondReceiver = incoming_msg.secondReceiver,
                        };
                        sendMessage( target_node, response_msg );
                        sendMessage( incoming_msg.secondReceiver, response_msg );

                        local_node.cache[ cache_slot ].state = INVALID;

                        break;

                    case FLUSH_INVACK:
                        if (current_thread == target_node) {
                            local_node.directory[mem_location].bitVector = (1 << incoming_msg.secondReceiver);
                            local_node.memory[mem_location] = incoming_msg.value;
                        }

                        if (current_thread == incoming_msg.secondReceiver) {
                            if (local_node.cache[ cache_slot ].address != incoming_msg.address &&
                                local_node.cache[ cache_slot ].state != INVALID) {
                                handleCacheReplacement( current_thread, local_node.cache[ cache_slot ] );
                            }
                            local_node.cache[ cache_slot ].address = incoming_msg.address;
                            local_node.cache[ cache_slot ].value = current_instr.value;
                            local_node.cache[ cache_slot ].state = MODIFIED;
                        }

                        awaiting_response = 0;
                        break;

                    case EVICT_SHARED:
                        if (current_thread != target_node) {
                            local_node.cache[ cache_slot ].state = EXCLUSIVE;
                        } else {
                            local_node.directory[mem_location].bitVector &= ~(1 << incoming_msg.sender);

                            int sharer_count = __builtin_popcount(local_node.directory[mem_location].bitVector);
                            if (sharer_count == 0) {
                                local_node.directory[mem_location].state = U;
                            } else if (sharer_count == 1) {
                                local_node.directory[mem_location].state = EM;

                                int new_owner = __builtin_ctz(local_node.directory[mem_location].bitVector);

                                if (new_owner != target_node) {
                                    incoming_msg = (message) {
                                        .type = EVICT_SHARED,
                                        .sender = current_thread,
                                        .address = incoming_msg.address,
                                        .value = local_node.memory[ mem_location ],
                                    };
                                    sendMessage( new_owner, incoming_msg );
                                } else {
                                    local_node.cache[ cache_slot ].state = EXCLUSIVE;
                                }
                            }
                        }
                        break;

                    case EVICT_MODIFIED:
                        local_node.memory[mem_location] = incoming_msg.value;
                        local_node.directory[mem_location].bitVector = 0;
                        local_node.directory[mem_location].state = U;
                        break;
                }
            }

            if ( awaiting_response > 0 ) {
                continue;
            }

            if ( instr_counter < local_node.instructionCount - 1 ) {
                instr_counter++;
            } else {
                if ( should_print > 0 ) {
                    printProcessorState( current_thread, local_node );
                    should_print--;
                }
                continue;
            }
            current_instr = local_node.instructions[ instr_counter ];

            byte target_proc = (current_instr.address >> 4) & 0x0F;
            byte mem_pos = current_instr.address & 0x0F;
            byte cache_pos = mem_pos % CACHE_SIZE;

            int cache_hit = local_node.cache[cache_pos].address == current_instr.address
                          ? local_node.cache[cache_pos].state == INVALID ? 0 : 1
                          : 0;

            if ( current_instr.type == 'R' ) {
                if (cache_hit) {
                    do { } while (false);
                } else {
                    incoming_msg = (message) {
                        .type = READ_REQUEST,
                        .sender = current_thread,
                        .address = current_instr.address,
                    };
                    sendMessage( target_proc, incoming_msg );
                    awaiting_response = 1;
                }
            } else {
                if (cache_hit) {
                    if (local_node.cache[cache_pos].state == MODIFIED ||
                        local_node.cache[cache_pos].state == EXCLUSIVE) {
                        local_node.cache[cache_pos].value = current_instr.value;
                        local_node.cache[cache_pos].state = MODIFIED;
                    } else {
                        incoming_msg = (message) {
                            .type = UPGRADE,
                            .sender = current_thread,
                            .address = current_instr.address,
                            .value = current_instr.value
                        };
                        sendMessage( target_proc, incoming_msg );
                        awaiting_response = 1;
                    }
                } else {
                    incoming_msg = (message) {
                        .type = WRITE_REQUEST,
                        .sender = current_thread,
                        .address = current_instr.address,
                        .value = current_instr.value
                    };
                    sendMessage( target_proc, incoming_msg );
                    awaiting_response = 1;
                }
            }
        }
    }
}

void sendMessage( int receiver, message msg ) {
    omp_set_lock( &msg_buffer_locks[ receiver ] );

    messageBuffer *msg_buf = &message_buffers[ receiver ];
    if (msg_buf->count < MSG_BUFFER_SIZE) {
      msg_buf->queue[ msg_buf->tail ] = msg;
      msg_buf->tail = (msg_buf->tail + 1 ) % MSG_BUFFER_SIZE;
      msg_buf->count++;
    } 

    omp_unset_lock( &msg_buffer_locks[ receiver ] );
}

void handleCacheReplacement( int sender, cacheLine old_cache_line ) {
    byte mem_pos = old_cache_line.address & 0x0F;
    byte target_proc = (old_cache_line.address >> 4) & 0x0F;
    message evict_msg;
    
    switch ( old_cache_line.state ) {
        case EXCLUSIVE:
        case SHARED:
            evict_msg = (message) {
                .type = EVICT_SHARED,
                .sender = sender,
                .address = old_cache_line.address,
            };
            sendMessage( target_proc, evict_msg );
            break;
        case MODIFIED:
            evict_msg = (message) {
                .type = EVICT_MODIFIED,
                .sender = sender,
                .address = old_cache_line.address,
                .value = old_cache_line.value,
            };
            sendMessage( target_proc, evict_msg );
            break;
        case INVALID:
            break;
    }
}
// --- IMPORTANT: DO NOT MODIFY --

void initializeProcessor( int threadId, processorNode *node, char *dirName ) {
    // IMPORTANT: DO NOT MODIFY
    for ( int i = 0; i < MEM_SIZE; i++ ) {
        node->memory[ i ] = 20 * threadId + i;  // some initial value to mem block
        node->directory[ i ].bitVector = 0;     // no cache has this block at start
        node->directory[ i ].state = U;         // this block is in Unowned state
    }

    for ( int i = 0; i < CACHE_SIZE; i++ ) {
        node->cache[ i ].address = 0xFF;        // this address is invalid as we can
                                                // have a maximum of 8 nodes in the 
                                                // current implementation
        node->cache[ i ].value = 0;
        node->cache[ i ].state = INVALID;       // all cache lines are invalid
    }

    // read and parse instructions from core_<threadId>.txt
    char filename[ 128 ];
    snprintf(filename, sizeof(filename), "tests/%s/core_%d.txt", dirName, threadId);
    FILE *file = fopen( filename, "r" );
    if ( !file ) {
        fprintf( stderr, "Error: count not open file %s\n", filename );
        exit( EXIT_FAILURE );
    }

    char line[ 20 ];
    node->instructionCount = 0;
    while ( fgets( line, sizeof( line ), file ) &&
            node->instructionCount < MAX_INSTR_NUM ) {
        if ( line[ 0 ] == 'R' && line[ 1 ] == 'D' ) {
            sscanf( line, "RD %hhx",
                    &node->instructions[ node->instructionCount ].address );
            node->instructions[ node->instructionCount ].type = 'R';
            node->instructions[ node->instructionCount ].value = 0;
        } else if ( line[ 0 ] == 'W' && line[ 1 ] == 'R' ) {
            sscanf( line, "WR %hhx %hhu",
                    &node->instructions[ node->instructionCount ].address,
                    &node->instructions[ node->instructionCount ].value );
            node->instructions[ node->instructionCount ].type = 'W';
        }
        node->instructionCount++;
    }

    fclose( file );
    printf( "Processor %d initialized\n", threadId );
}

void printProcessorState(int processorId, processorNode node) {
    // IMPORTANT: DO NOT MODIFY
    static const char *cacheStateStr[] = { "MODIFIED", "EXCLUSIVE", "SHARED",
                                           "INVALID" };
    static const char *dirStateStr[] = { "EM", "S", "U" };

    char filename[32];
    snprintf(filename, sizeof(filename), "core_%d_output.txt", processorId);

    FILE *file = fopen(filename, "w");
    if (!file) {
        printf("Error: Could not open file %s\n", filename);
        return;
    }

    fprintf(file, "=======================================\n");
    fprintf(file, " Processor Node: %d\n", processorId);
    fprintf(file, "=======================================\n\n");

    // Print memory state
    fprintf(file, "-------- Memory State --------\n");
    fprintf(file, "| Index | Address |   Value  |\n");
    fprintf(file, "|----------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %5d   |\n", i, (processorId << 4) + i,
                node.memory[i]);
    }
    fprintf(file, "------------------------------\n\n");

    // Print directory state
    fprintf(file, "------------ Directory State ---------------\n");
    fprintf(file, "| Index | Address | State |    BitVector   |\n");
    fprintf(file, "|------------------------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %2s   |   0x%08B   |\n",
                i, (processorId << 4) + i, dirStateStr[node.directory[i].state],
                node.directory[i].bitVector);
    }
    fprintf(file, "--------------------------------------------\n\n");
    
    // Print cache state
    fprintf(file, "------------ Cache State ----------------\n");
    fprintf(file, "| Index | Address | Value |    State    |\n");
    fprintf(file, "|---------------------------------------|\n");
    for (int i = 0; i < CACHE_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %3d  |  %8s \t|\n",
               i, node.cache[i].address, node.cache[i].value,
               cacheStateStr[node.cache[i].state]);
    }
    fprintf(file, "----------------------------------------\n\n");

    fclose(file);
}

// --- IMPORTANT: DO NOT MODIFY --