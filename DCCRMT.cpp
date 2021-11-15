/*
 *  © 2021, Harald Barth.
 *  
 *  This file is part of DCC-EX
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "defines.h"
#include "DIAG.h"
#include "DCCRMT.h"

//#include "soc/periph_defs.h"
//#include "driver/periph_ctrl.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(4,2,0)
#error wrong IDF version
#endif

void setDCCBit1(rmt_item32_t* item) {
  item->level0    = 1;
  item->duration0 = DCC_1_HALFPERIOD;
  item->level1    = 0;
  item->duration1 = DCC_1_HALFPERIOD;
}

void setDCCBit0(rmt_item32_t* item) {
  item->level0    = 1;
  item->duration0 = DCC_0_HALFPERIOD;
  item->level1    = 0;
  item->duration1 = DCC_0_HALFPERIOD;
}

void setDCCBit0Last(rmt_item32_t* item) {
  item->level0    = 1;
  item->duration0 = DCC_0_HALFPERIOD + DCC_0_HALFPERIOD/10;
  item->level1    = 0;
  item->duration1 = DCC_0_HALFPERIOD;
}

void setEOT(rmt_item32_t* item) {
  item->val = 0;
}

void IRAM_ATTR interrupt(rmt_channel_t channel, void *t) {
  RMTPin *tt = (RMTPin *)t;
  tt->RMTinterrupt();
}

RMTPin::RMTPin(byte pin, byte ch, byte plen) {

  // preamble
  preambleLen = plen+2; // plen 1 bits, one 0 bit and one EOF marker
  preamble = (rmt_item32_t*)malloc(preambleLen*sizeof(rmt_item32_t));
  for (byte n=0; n<plen; n++)
    setDCCBit1(preamble + n);      // preamble bits
  setDCCBit0(preamble + plen); // start of packet 0 bit
  setEOT(preamble + plen + 1);     // EOT marker

  // idle
  idleLen = 29;
  idle = (rmt_item32_t*)malloc(idleLen*sizeof(rmt_item32_t));
  for (byte n=0; n<8; n++)   // 0 to 7
    setDCCBit1(idle + n);
  for (byte n=8; n<18; n++)  // 8, 9 to 16, 17
    setDCCBit0(idle + n);
  for (byte n=18; n<26; n++) // 18 to 25
    setDCCBit1(idle + n);
  setDCCBit1(idle + 26);     // end bit
  setDCCBit0Last(idle + 27); // finish always with 0
  setEOT(idle + 28);         // EOT marker

  // data: max packet size today is 5 + checksum
  dataLen = (5+1)*9+2; // Each byte has one bit extra and one 0 bit and one EOF marker
  data = (rmt_item32_t*)malloc(dataLen*sizeof(rmt_item32_t));
  
  rmt_config_t config;
  // Configure the RMT channel for TX
  bzero(&config, sizeof(rmt_config_t));
  config.rmt_mode = RMT_MODE_TX;
  config.channel = channel = (rmt_channel_t)ch;
  config.clk_div = 1;             // use 80Mhz clock directly
  config.gpio_num = (gpio_num_t)pin;
  config.mem_block_num = 2; // With longest DCC packet 11 inc checksum (future expansion)
                            // number of bits needed is 22preamble + start +
                            // 11*9 + extrazero + EOT = 124
                            // 2 mem block of 64 RMT items should be enough

  // this was not our problem https://esp32.com/viewtopic.php?t=5252
  //periph_module_disable(PERIPH_RMT_MODULE);
  //periph_module_enable(PERIPH_RMT_MODULE);

  ESP_ERROR_CHECK(rmt_config(&config));
  // NOTE: ESP_INTR_FLAG_IRAM is *NOT* included in this bitmask
  ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, ESP_INTR_FLAG_LOWMED|ESP_INTR_FLAG_SHARED));

  DIAG(F("Register interrupt on core %d"), xPortGetCoreID());

  rmt_register_tx_end_callback(interrupt, this);
  rmt_set_tx_intr_en(channel, true);

  // rmt_set_source_clk() // not needed as APB only supported currently
  

  //rmt_register_tx_end_callback()
    
  DIAG(F("Starting channel %d signal generator"), config.channel);

  // send one bit to kickstart the signal, remaining data will come from the
  // packet queue. We intentionally do not wait for the RMT TX complete here.
  //rmt_write_items(channel, preamble, preambleLen, false);
  RMTprefill();
  preambleNext = true;
  dataReady = false;
  RMTinterrupt();
}

void RMTPin::RMTprefill() {
  rmt_fill_tx_items(channel, preamble, preambleLen, 0);
  rmt_fill_tx_items(channel, idle, idleLen, preambleLen-1);
}

const byte transmitMask[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

bool RMTPin::fillData(const byte buffer[], byte byteCount, byte repeatCount=1) {
  if (dataReady == true || dataRepeat > 0) // we have still old work to do
    return false;
  byte bitcounter = 0;
  for(byte n=0; n<byteCount; n++) {
    for(byte bit=0; bit<8; bit++) {
      if (buffer[n] & transmitMask[bit])
	setDCCBit1(data + bitcounter++);
      else
	setDCCBit0(data + bitcounter++);
    }
    setDCCBit0(data + bitcounter++); // zero at end of each byte
  }
  setDCCBit1(data + bitcounter-1);     // overwrite previous zero bit with one bit
  setDCCBit0Last(data + bitcounter++); // extra 0 bit after end bit
  setEOT(data + bitcounter++);         // EOT marker
  dataLen = bitcounter;
  dataReady = true;
  dataRepeat = repeatCount;
  return true;
}

void IRAM_ATTR RMTPin::RMTinterrupt() {
  rmt_tx_start(channel,true);
  /*  byte foo[3];
  foo[0] = 0xF0;
  foo[1] = 0x0F;
  foo[2] = 0xAA;
  fillData(foo, 3);*/
  if (dataReady) {
    rmt_fill_tx_items(channel, data, dataLen, preambleLen-1);
    dataReady = false;
  }
  if (dataRepeat > 0)
    dataRepeat--;
  return;
}
