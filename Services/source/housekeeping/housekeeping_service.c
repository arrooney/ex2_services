/*
 * Copyright (C) 2015  University of Alberta
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
/**
 * @file housekeeping_service.c
 * @author Haoran Qi, Andrew Rooney, Yuan Wang, Dustin Wagner
 * @date 2020-07-07
 */
#include "housekeeping/housekeeping_service.h"

#include <FreeRTOS.h>
#include <os_task.h>
#include <os_semphr.h>
#include <csp/csp.h>
#include <csp/csp_endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/service_utilities.h"
#include "services.h"

static uint8_t SID_byte = 1;

/*for housekeeping temporary file creation
  naming convention becomes base_file + current_file + extension
  e.g. tempHKdata134.TMP
*/
uint16_t MAX_FILES = 500; //currently arbitrary number
char base_file[] = "tempHKdata"; //path may need to be changed
char extension[] = ".TMP";
uint16_t current_file = 1;  //Increments after file write. loops back at MAX_FILES
                            //1 indexed

uint32_t *timestamps; //This is a dynamic array to handle file search by timestamp
uint16_t hk_timestamp_array_size = 0; //NOT BYTES. stored as number of items. 1 indexed. 0 element unused

SemaphoreHandle_t f_count_lock;

/**
 * @brief
 *      gets the hk file id that holds a timestamp closest to that given
 * @attention
 *      Complex algorithm. should be thoroughly tested when possible
 * @param timestamp
 *      This is the time from which the file is desired
 * @return uint16_t
 *      File ID if found. 0 if no file found
 */
uint16_t get_file_id_from_timestamp(uint32_t timestamp) {
  uint32_t threshold = 15; //How many seconds timestamps need to be within. Currently assumes 30 second hk intervals
  //perform leftmost binary search
  uint16_t left;
  uint16_t right;
  uint16_t middle;
  if(timestamps == NULL) {
    return 0;
  }
  if (timestamps[current_file] == 0) { //haven't made full loop of storage
    if (current_file == 1) { //base case. no files written
      return 0;
    }
    //not enough files written to use loop portion yet
    left = 1;
    right = current_file -1;
  } else {
    //These accomodate circular structure
    left = current_file;
    right = left + hk_timestamp_array_size - 1;
  }

  while (left < right) {
    middle = (left + right) / 2;
    if (timestamps[middle - (current_file - 1)] < timestamp) {
      left = middle + 1;
    } else {
      right = middle;
    }
  }
  uint16_t true_position = left - (current_file - 1);
  if (true_position > 1) {
    if (timestamp - timestamps[true_position - 1] <= threshold) { //check lower neighbour if exists
      return true_position - 1;
    }
    if (true_position != hk_timestamp_array_size) { //check self if won't cause underflow
      if (timestamps[true_position] - timestamp <= threshold) {
        return true_position;
      }
    } else { //left must be max index
      if (timestamp > timestamps[true_position] && 
      timestamp - timestamps[true_position] <= threshold) { //edge case left is max index. bigger value than at max
        return true_position;
      } else if (timestamps[true_position] - timestamp <= threshold) { //edge case left is max index. smaller value than at max
        return true_position;
      }
    }
  } else { //left must be min index
    if (timestamps[true_position] - timestamp <= threshold) { //edge case left is min index. smaller than min
      return true_position;
    }
  }
  return 0;


}

/**
 * @brief
 *      Handles malloc, realloc, and free for array holding hk timestamps
 * @param num_items
 *      This is how many items should have space malloced
 * @return Result
 *      FAILURE or SUCCESS
 */
Result dynamic_timestamp_array_handler(uint16_t num_items) {
  if (num_items == 0) {
    if (timestamps != NULL) {
      free(timestamps);
    }
    hk_timestamp_array_size = 0;
    return SUCCESS;
  }
  if (num_items != hk_timestamp_array_size) {
    uint32_t *tmp = realloc(timestamps, sizeof(*timestamps * (num_items + 1))); //+1 to allow non-zero index room
    if (!tmp) { 
      return FAILURE;
    }
    timestamps = tmp;
    uint16_t i = hk_timestamp_array_size + 1;
    hk_timestamp_array_size = num_items;

    for (i; i <= num_items; i++) {
      timestamps[i] = 0;
    }
  }
  return SUCCESS;
}



/**
 * @brief
 *      Private. Collect housekeeping information from each device in system
 * @attention
 *      Error testing and review recommended to check for possible shallow
 *      copies and resultant data loss and concurrency errors
 * @param all_hk_data
 *      pointer to struct of all the housekeeping data collected from components
 * @return Result
 *      FAILURE or SUCCESS
 */
Result collect_hk_from_devices(All_systems_housekeeping* all_hk_data) {
  /*populate struct by calling appropriate functions*/
  //TODO:
  //ADCS_return = HAL_adcs_gethk(&all_hk_data->ADCS_hk);             //ADCS get housekeeeing
  int Athena_return_code = Athena_getHK(&all_hk_data->Athena_hk);    //Athena get temperature

  EPS_getHK(&all_hk_data->EPS_hk);                                   //EPS get housekeeping
  UHF_return UHF_return_code = UHF_getHK(&all_hk_data->UHF_hk);      //UHF get housekeeping
  STX_return STX_return_code = HAL_S_getHK(&all_hk_data->S_band_hk); //S_band get housekeeping
  /*consider if struct should hold error codes returned from these functions*/
  return SUCCESS;
}

/**
 * @brief
 *      Check if file with given name exists
 * @param filename
 *      const char * to name of file to check
 * @return Found_file
 *      FILE_EXISTS or FILE_NOT_EXIST
 */
Found_file exists(const char *filename){
    FILE *file;
    if((file = fopen(filename, "rb"))){ //open file to read binary
        fclose(file);
        return FILE_EXISTS;
    }
    return FILE_NOT_EXIST;
}

/**
 * @brief
 *      Write housekeeping data to the given file
 * @details
 *      Writes one struct to file for each subsystem present
 *      Order of writes must match the appropriate read function
 * @param filename
 *      const char * to name of file to write to
 * @param all_hk_data
 *      Struct containing structs of other hk data
 * @return Result
 *      FAILURE or SUCCESS
 */
Result write_hk_to_file(const char *filename, All_systems_housekeeping* all_hk_data) {
  FILE *fout = fopen(filename, "wb"); //open or create file to write binary
  if (fout == NULL) {
    ex2_log("Failed to open or create file to write: '%c'\n", filename);
    return FAILURE;
  }
  /*The order of writes and subsequent reads must match*/
  //TODO:
  //fwrite(&all_hk_data->ADCS_hk, sizeof(all_hk_data->ADCS_hk), 1, fout);
  fwrite(&all_hk_data->hk_timeorder, sizeof(all_hk_data->hk_timeorder), 1, fout);
  fwrite(&all_hk_data->Athena_hk, sizeof(all_hk_data->Athena_hk), 1, fout);
  fwrite(&all_hk_data->EPS_hk, sizeof(all_hk_data->EPS_hk), 1, fout);
  fwrite(&all_hk_data->UHF_hk, sizeof(all_hk_data->UHF_hk), 1, fout);
  fwrite(&all_hk_data->S_band_hk, sizeof(all_hk_data->S_band_hk), 1, fout);

  if(fwrite == 0) {
    ex2_log("Failed to write to file: '%c'\n", filename);
    return FAILURE;
  }
  fclose(fout);
  return SUCCESS;
}

/**
 * @brief
 *      Read housekeeping data from given file
 * @details
 *      Reads one struct from file for each subsystem present
 *      Order of reads must match the appropriate write function
 * @param filename
 *      const char * to name of file to read from
 * @param all_hk_data
 *      Struct containing structs of other hk data
 * @return Result
 *      FAILURE or SUCCESS
 */
Result read_hk_from_file(const char *filename, All_systems_housekeeping* all_hk_data) {
  if(!exists(filename)){
    ex2_log("Attempted to read file that doesn't exist: '%c'\n", filename);
    return FAILURE;
  }
  FILE *fin = fopen(filename, "rb"); //open file to read binary
  if (fin == NULL) {
    ex2_log("Failed to open file to read: '%c'\n", filename);
    return FAILURE;
  }
  /*The order of writes and subsequent reads must match*/
  //TODO:
  //fread(&all_hk_data->ADCS_hk, sizeof(all_hk_data->ADCS_hk), 1, fin);
  fread(&all_hk_data->hk_timeorder, sizeof(all_hk_data->hk_timeorder), 1, fin);
  fread(&all_hk_data->Athena_hk, sizeof(all_hk_data->Athena_hk), 1, fin);
  fread(&all_hk_data->EPS_hk, sizeof(all_hk_data->EPS_hk), 1, fin);
  fread(&all_hk_data->UHF_hk, sizeof(all_hk_data->UHF_hk), 1, fin);
  fread(&all_hk_data->S_band_hk, sizeof(all_hk_data->S_band_hk), 1, fin);

  return SUCCESS;
}

/*Helper function to find number of digits in number*/
int num_digits(int num) {
  uint16_t count = 0;

  while(num != 0) {
    num /= 10;
    ++count;
  }
  return count;
}

static SemaphoreHandle_t prv_get_count_lock() {
  if (!f_count_lock) {
    f_count_lock = xSemaphoreCreateMutex();
  }
  configASSERT(f_count_lock);
  return &f_count_lock;
}

static inline void prv_get_lock(SemaphoreHandle_t *lock) {
  configASSERT(lock);
  xSemaphoreTake(lock, portMAX_DELAY);
}

static inline void prv_give_lock(SemaphoreHandle_t *lock) {
  configASSERT(lock);
  xSemaphoreGive(lock);
}

/**
 * @brief
 *      Public. Performs all calls and operations to retrieve hk data and store it
 * @return
 *      enum for SUCCESS or FAILURE
 */
Result populate_and_store_hk_data(void) {
  All_systems_housekeeping temp_hk_data;

  if(collect_hk_from_devices(&temp_hk_data) == FAILURE) {
    ex2_log("Error collecting hk data from peripherals\n");
  }

  //Not sure if time works like a normal machine but can be changed
  temp_hk_data.hk_timeorder.UNIXtimestamp = (uint32_t)time(NULL); //set creation time to now
  SemaphoreHandle_t lock = prv_get_count_lock();

  prv_get_lock(lock); //lock
  configASSERT(lock);
  temp_hk_data.hk_timeorder.dataPosition = current_file;

  uint16_t length = strlen(base_file) + num_digits(current_file) + 
  strlen(extension) + 1;
  char * filename = malloc(length); // + 1 for NULL terminator

  if(!filename) {
    ex2_log("Error, failed to malloc %hu bytes\n", length);
    return FAILURE;
  }
  snprintf(filename, length, "%s%hu%s", base_file, current_file, extension);

  if(!write_hk_to_file(filename, &temp_hk_data)) {
    ex2_log("Housekeeping data lost\n");
    free(filename); // No memory leaks here
    return FAILURE;
  }


  if (dynamic_timestamp_array_handler(MAX_FILES)) {
    timestamps[current_file] = temp_hk_data.hk_timeorder.UNIXtimestamp;
  } else {
    ex2_log("Warning, failed to malloc for secondary data structure\n");
  }

  ++current_file;

  if(current_file > MAX_FILES) {
    current_file = 1;
  }
  prv_give_lock(lock); //unlock

  free(filename); // No memory leaks here
  return SUCCESS;
}

/**
 * @brief
 *      Performs all calls and operations to load hk data from disk
 * @param file_num
 *      The id of the file to be retrieved. will be combined into full
 *      file name and is checked to ensure request is valid
 * @param all_hk_data
 *      Struct containing structs of other hk data
 * @return
 *      enum for SUCCESS or FAILURE
 */
Result load_historic_hk_data(uint16_t file_num, All_systems_housekeeping* all_hk_data) {
  uint16_t length = strlen(base_file) + num_digits(current_file) + 
  strlen(extension) + 1;
  char * filename = malloc(length); // + 1 for NULL terminator

  if(!filename) {
    ex2_log("Error, failed to malloc %hu bytes\n", length);
    return FAILURE;
  }
  snprintf(filename, length, "%s%hu%s", base_file, current_file, extension);

  if(!read_hk_from_file(filename, all_hk_data)) {
    ex2_log("Housekeeping data could not be retrieved\n");
    free(filename); // No memory leaks here
    return FAILURE;
  }

  free(filename); // No memory leaks here
  return SUCCESS;
}

/**
 * @brief
 *      Change the maximum number of files stored by housekeeping service
 * @attention
 *      If new_max is less than MAX_FILES, all historic housekeeping files will
 *      be destroyed immediately to prevent orphaned files and confusion of data
 *      order. The next file written to after this function will be file #1.
 *      If new_max is greater than MAX_FILES, the data flow will be unaffected.
 * @param new_max
 *      The new value to change the maximum value to  
 * @return
 *      enum for SUCCESS or FAILURE
 */
Result set_max_files(uint16_t new_max) {
  //ensure number requested isn't garbage
  if (new_max < 1) return FAILURE;

  SemaphoreHandle_t lock = prv_get_count_lock();
  prv_get_lock(lock); //lock
  configASSERT(lock);
  
  //adjust the array

  //ensure value set before cleanup
  uint16_t old_max = MAX_FILES;
  MAX_FILES = new_max;
  current_file = 1;

  prv_give_lock(lock); //unlock
  
  if (old_max < new_max) return SUCCESS;

  //Cleanup files code if number of files has been reduced
  uint16_t length = strlen(base_file) + num_digits(old_max) + 
  strlen(extension) + 1;
  char * filename = malloc(length); // + 1 for NULL terminator

  if(!filename) {
    ex2_log("Error, failed to malloc %hu bytes\n", length);
    return FAILURE;
  }
  Result result = SUCCESS;
  uint16_t i = 0;
  for (i = 1; i <= old_max; i++) {
    snprintf(filename, length, "%s%hu%s", base_file, i, extension);
    if (exists(filename) == FILE_EXISTS) {
      if (!remove(filename) && i > new_max){
        ex2_log("Error, file %s has been orphaned\n", filename);
        result = FAILURE;
      }
    }
  }
  return result;
}

/**
 * @brief
 *      Is given a struct of all the housekeeping data and converts the
 *      endianness of each value to be sent over the network
 * @param hk
 *      A struct of all the housekeeping data
 * @return
 *      enum for SUCCESS or FAILURE
 */
Result convert_hk_endianness(All_systems_housekeeping* hk){
  /*hk_time_and_order*/
  hk->hk_timeorder.UNIXtimestamp = csp_hton32(hk->hk_timeorder.UNIXtimestamp);
  hk->hk_timeorder.dataPosition = csp_hton16(hk->hk_timeorder.dataPosition);

  //TODO:
  //hk->ADCS_hk.
  
  /*athena_housekeeping*/
  Athena_hk_convert_endianness(&hk->Athena_hk);

  /*eps_instantaneous_telemetry_t*/
  prv_instantaneous_telemetry_letoh(&hk->EPS_hk);
  
  /*UHF_housekeeping*/
  UHF_convert_endianness(&hk->UHF_hk);
  
  /*Sband_Housekeeping*/
  HAL_S_hk_convert_endianness(&hk->S_band_hk);

  return SUCCESS;
}

/**
 * @brief
 *      Paging function to retrieve sets of data so they can be transmitted
 * @param conn
 *      Pointer to the connection on which to send packets
 * @param limit
 *      Maximum number of housekeeping files to retrieve in this request
 * @param before_id
 *      The earliest file in time that the user received. (lowest id)
 *      Files older than before_id will be fetched.
 *      Functions like a typical web API for paging. Prevents page drift.
 *      0 value means ignore variable. retrieve from most recent
 * @return
 *      enum for success or failure
 */
Result fetch_historic_hk_and_transmit(csp_conn_t *conn, uint16_t limit, uint16_t before_id, uint32_t before_time) {
  SemaphoreHandle_t lock = prv_get_count_lock();
  prv_get_lock(lock);
  configASSERT(lock);
  uint16_t locked_max = MAX_FILES;
  uint16_t locked_before_id = before_id;
  uint32_t locked_before_time = before_time;
  if (locked_before_time != 0){ //use timestamp if exists
    locked_before_id = get_file_id_from_timestamp(locked_before_time);
  }
  prv_give_lock(lock);

  //error check and accomodate user input
  if (locked_before_id == 0 || locked_before_id > locked_max) {
    locked_before_id = current_file;
  }
  //Check for data limit ignorance
  if (limit > locked_max){
    limit = (uint16_t)locked_max;
  } else if (limit == 0) {
    ex2_log("Successfully did nothing O_o");
    return SUCCESS;
  }

  //fetch each appropriate set of data from file
  while (limit > 0) {
    locked_before_id--;

    if (locked_before_id == 0) {
      locked_before_id = locked_max;
    }
    All_systems_housekeeping* all_hk_data = {0};
    if (load_historic_hk_data(locked_before_id, all_hk_data) != SUCCESS) {
      return FAILURE;
    }
    if (convert_hk_endianness(all_hk_data) != SUCCESS) {
      return FAILURE;
    }
    int8_t status = 0;
    
    //needed_size is currently 354 bytes as of 2021/05/25
    //TODO:
    //sizeof(all_hk_data->ADCS_hk) +
    uint16_t needed_size =  sizeof(all_hk_data->hk_timeorder) + //currently 6U
                            sizeof(all_hk_data->Athena_hk) +    //currently 24U
                            sizeof(all_hk_data->EPS_hk) +       //currently 236U
                            sizeof(all_hk_data->UHF_hk) +       //currently 55U
                            sizeof(all_hk_data->S_band_hk) +    //currently 32U
                            1;
      
    csp_packet_t *packet = csp_buffer_get(needed_size);
    uint8_t ser_subtype = GET_HK;
    memcpy(&packet->data[SUBSERVICE_BYTE], &ser_subtype, sizeof(int8_t));

    uint16_t used_size = 0;
    memcpy(&packet->data[STATUS_BYTE], &status, sizeof(int8_t));
    memcpy(&packet->data[OUT_DATA_BYTE + used_size], &all_hk_data->hk_timeorder, sizeof(all_hk_data->hk_timeorder));
    used_size += sizeof(all_hk_data->hk_timeorder);
    //TODO:
    //memcpy(&packet->data[OUT_DATA_BYTE + used_size], &all_hk_data->ADCS_hk, sizeof(all_hk_data->ADCS_hk));
    //used_size += sizeof(all_hk_data->ADCS_hk);
    memcpy(&packet->data[OUT_DATA_BYTE + used_size], &all_hk_data->Athena_hk, sizeof(all_hk_data->Athena_hk));
    used_size += sizeof(all_hk_data->Athena_hk);
    memcpy(&packet->data[OUT_DATA_BYTE + used_size], &all_hk_data->EPS_hk, sizeof(all_hk_data->EPS_hk));
    used_size += sizeof(all_hk_data->EPS_hk);
    memcpy(&packet->data[OUT_DATA_BYTE + used_size], &all_hk_data->UHF_hk, sizeof(all_hk_data->UHF_hk));
    used_size += sizeof(all_hk_data->UHF_hk);
    memcpy(&packet->data[OUT_DATA_BYTE + used_size], &all_hk_data->S_band_hk, sizeof(all_hk_data->S_band_hk));
    used_size += sizeof(all_hk_data->S_band_hk);
    set_packet_length(packet, used_size + 1);
    
    if (!csp_send(conn, packet, 50)) { //why are we all using magic number?
      ex2_log("Failed to send packet");
      csp_buffer_free(packet);
      return FAILURE;
    }
    csp_buffer_free(packet);
    limit--;
  }

  return SUCCESS;
}

/**
 * @brief
 *      Processes the incoming requests to decide what response is needed
 * @param conn
 *      Pointer to the connection on which to receive and send packets
 * @param packet
 *      The packet that was sent from the ground station
 * @return
 *      enum for return state
 */
SAT_returnState hk_service_app(csp_conn_t *conn, csp_packet_t *packet) {
  uint8_t ser_subtype = (uint8_t)packet->data[SUBSERVICE_BYTE];
  int8_t status;
  uint16_t new_max_files;
  uint16_t limit;
  uint16_t before_id;
  uint32_t before_time;

  switch (ser_subtype) {
    case SET_MAX_FILES:
      cnv8_16(&packet->data[IN_DATA_BYTE], &new_max_files);
      new_max_files = csp_ntoh16(new_max_files);

      if (set_max_files(new_max_files) != SUCCESS) {
        status = -1;
        memcpy(&packet->data[STATUS_BYTE], &status, sizeof(int8_t));
      } else {
        status = 0;
        memcpy(&packet->data[STATUS_BYTE], &status, sizeof(int8_t));
      }

      set_packet_length(packet, sizeof(int8_t) + 1);  // +1 for subservice

      if (!csp_send(conn, packet, 50)) {
        csp_buffer_free(packet);
      }
      break;
    
    case GET_MAX_FILES:
      new_max_files = MAX_FILES;
      new_max_files = csp_hton16(new_max_files);
      status = 0;
      memcpy(&packet->data[STATUS_BYTE], &status, sizeof(int8_t));
      memcpy(&packet->data[OUT_DATA_BYTE], &new_max_files, sizeof(new_max_files));

      set_packet_length(packet, sizeof(int8_t)
                              + sizeof(new_max_files)
                              + 1);  // +1 for subservice

      if (!csp_send(conn, packet, 50)) {
        csp_buffer_free(packet);
      }
      break;

    case GET_HK:
      limit = (uint16_t)packet->data16[IN_DATA_BYTE];
      before_id = (uint16_t)packet->data16[IN_DATA_BYTE + 1];
      before_time = (uint32_t)packet->data16[IN_DATA_BYTE + 2];

      if (fetch_historic_hk_and_transmit(conn, limit, before_id, before_time) != SUCCESS) {
        return SATR_ERROR;
      }
      break;

    default:
      ex2_log("No such subservice\n");
      return SATR_PKT_ILLEGAL_SUBSERVICE;
  }


  

  return SATR_OK;
}

SAT_returnState start_housekeeping_service(void);

/**
 * @brief
 *      FreeRTOS housekeeping server task
 * @details
 *      Accepts incoming housekeeping service packets and executes the application
 * @param void* param
 * @return None
 */
void housekeeping_service(void * param) {
    csp_socket_t *sock;
    sock = csp_socket(CSP_SO_RDPREQ);
    csp_bind(sock, TC_HOUSEKEEPING_SERVICE);
    csp_listen(sock, SERVICE_BACKLOG_LEN);

    for(;;) {
        csp_conn_t *conn;
        csp_packet_t *packet;
        if ((conn = csp_accept(sock, CSP_MAX_TIMEOUT)) == NULL) {
          /* timeout */
          continue;
        }
        while ((packet = csp_read(conn, 50)) != NULL) {
          /*
          if (hk_service_app(packet) != SATR_OK) {
            // something went wrong, this shouldn't happen
            csp_buffer_free(packet);
          } else {
              if (!csp_send(conn, packet, 50)) {
                  csp_buffer_free(packet);
              }
          }
          */
          if (hk_service_app(conn, packet) != SATR_OK) {
            ex2_log("Error responding to packet");
          }
        }
        csp_close(conn); //frees buffers used
    }
}

/**
 * @brief
 *      Start the housekeeping server task
 * @details
 *      Starts the FreeRTOS task responsible for accepting incoming
 *      housekeeping service requests
 * @param None
 * @return SAT_returnState
 *      success report
 */
SAT_returnState start_housekeeping_service(void) {
  if (xTaskCreate((TaskFunction_t)housekeeping_service,
                  "start_housekeeping_service", 300, NULL, NORMAL_SERVICE_PRIO,
                  NULL) != pdPASS) {
    ex2_log("FAILED TO CREATE TASK start_housekeeping_service\n");
    return SATR_ERROR;
  }
  ex2_log("Service handlers started\n");
  return SATR_OK;
}

/*
static SAT_returnState hk_parameter_report(csp_packet_t *packet);

SAT_returnState hk_service_app(csp_packet_t *packet) {
  uint8_t ser_subtype = (uint8_t)packet->data[SUBSERVICE_BYTE];

  switch (ser_subtype) {
    case TM_HK_PARAMETERS_REPORT:
      if (hk_parameter_report(packet) != SATR_OK) {
        ex2_log("HK_REPORT_PARAMETERS failed");
        return SATR_ERROR;
      }
      break;
    default:
      ex2_log("HK SERVICE NOT FOUND SUBTASK");
      return SATR_PKT_ILLEGAL_SUBSERVICE;
  }

  return SATR_OK;
}

static SAT_returnState hk_parameter_report(csp_packet_t *packet) {
  size_t size =
      HAL_hk_report(packet->data[SID_byte], packet->data + SID_byte + 1);

  set_packet_length(packet, size + 2);  // plus one for sub-service + SID

  return SATR_OK;
}
*/
