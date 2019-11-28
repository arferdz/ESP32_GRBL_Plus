#include <FS.h>
#include <SPIFFS.h>
#include <lvgl.h>
#include <Ticker.h>
#include <TFT_eSPI.h>
#include "grbl.h"
#include "terminal.h"
#include "wificonfig.h"

#define FORMAT_SPIFFS_IF_FAILED true
#define TERMINAL_ANIM_TIME 100
#define TERMINAL_NO_INPUT 0
#define TERMINAL_LOG_LENGTH 512

TFT_eSPI tft = TFT_eSPI(); /* TFT instance */
int screenWidth = 480;
int screenHeight = 320;

char GCode[]= {""};

static const int buzzerPin = 1 ;

static lv_obj_t * winterm;
static char txt_log[TERMINAL_LOG_LENGTH + 1];
static lv_obj_t * terminal_label;

#define LVGL_TICK_PERIOD 120 // default is 60
Ticker tick; /* timer for interrupt handler */
static lv_disp_buf_t disp_buf;
static lv_color_t buf[LV_HOR_RES_MAX * 10];

#if USE_LV_LOG != 0 /* Serial debugging */
void my_print(lv_log_level_t level, const char * file, uint32_t line, const char * dsc)
    {
    Serial.printf("%s@%d->%s\r\n", file, line, dsc);
    delay(100);
    }
#endif

/* 
Display flushing 
*/
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
    {
    uint16_t c;
    tft.startWrite(); /* Start new TFT transaction */
    tft.setAddrWindow(area->x1, area->y1, (area->x2 - area->x1 + 1), (area->y2 - area->y1 + 1)); /* set the working window */
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
        c = color_p->full;
        tft.writeColor(c, 1);
        color_p++;
        }
    }
    tft.endWrite(); /* terminate TFT transaction */
    lv_disp_flush_ready(disp); /* tell lvgl that flushing is done */
    }

/* 
Beep
*/
void beep(){
  digitalWrite(buzzerPin, HIGH);
  delay (100);
  digitalWrite(buzzerPin, LOW);  
    }  

/* 
Create Serial Console
*/
lv_obj_t * terminal_create(void){
    static lv_style_t style_bg;
    lv_style_copy(&style_bg, &lv_style_pretty);
    style_bg.body.main_color = LV_COLOR_BLACK;
    style_bg.body.grad_color = lv_color_make(0x30, 0x30, 0x30);
    style_bg.body.border.color = LV_COLOR_WHITE;
    style_bg.text.color = lv_color_make(0xE0, 0xE0, 0xE0);

    lv_coord_t hres = 250;
    lv_coord_t vres = 100;

    winterm = lv_win_create(lv_scr_act(), NULL);
    lv_win_set_style(winterm, LV_WIN_STYLE_BG, &style_bg);
    lv_obj_set_size(winterm, hres, vres);
    lv_win_set_sb_mode(winterm, LV_SB_MODE_OFF);
    lv_obj_align (winterm, NULL,LV_ALIGN_CENTER, 0, 55);

    /*Make the window's content responsive*/
    lv_win_set_layout(winterm, LV_LAYOUT_PRETTY);

    /*Create a label for the text of the terminal*/
    terminal_label = lv_label_create(winterm, NULL);
    lv_label_set_long_mode(terminal_label, LV_LABEL_LONG_EXPAND);
    lv_obj_set_width(terminal_label, 200);
    lv_label_set_static_text(terminal_label, txt_log);               /*Use the text array directly*/

    return winterm;
}
/* 
Add Lines to terminal console
*/
void terminal_add(const char * txt_in){
    if(winterm == NULL) return;                 /*Check if the window is exists*/

    uint16_t txt_len = strlen(txt_in);
    uint16_t old_len = strlen(txt_log);

    /*If the data is longer then the terminal ax size show the last part of data*/
    if(txt_len > TERMINAL_LOG_LENGTH) {
        txt_in += (txt_len - TERMINAL_LOG_LENGTH);
        txt_len = TERMINAL_LOG_LENGTH;
        old_len = 0;
    }
    /*If the text become too long 'forget' the oldest lines*/
    else if(old_len + txt_len > TERMINAL_LOG_LENGTH) {
        uint16_t new_start;
        for(new_start = 0; new_start < old_len; new_start++) {
            if(txt_log[new_start] == '\n') {
                /*If there is enough space break*/
                if(new_start >= txt_len) {
                    /*Ignore line breaks*/
                    while(txt_log[new_start] == '\n' || txt_log[new_start] == '\r') new_start++;
                    break;
                }
            }
        }

        /* If it wasn't able to make enough space on line breaks
         * simply forget the oldest characters*/
        if(new_start == old_len) {
            new_start = old_len - (TERMINAL_LOG_LENGTH - txt_len);
        }
        /*Move the remaining text to the beginning*/
        uint16_t j;
        for(j = new_start; j < old_len; j++) {
            txt_log[j - new_start] = txt_log[j];
        }
        old_len = old_len - new_start;
        txt_log[old_len] = '\0';

    }

    memcpy(&txt_log[old_len], txt_in, txt_len);
    txt_log[old_len + txt_len] = '\0';

    lv_label_set_static_text(terminal_label, txt_log);
}

bool my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data){
    uint16_t touchX, touchY;
    bool touched = tft.getTouch(&touchX, &touchY, 300);
    if(!touched)
    {
      return false;
    }
      data->state = touched ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL; 
      /*Save the state and save the pressed coordinate*/
      //if(data->state == LV_INDEV_STATE_PR) touchpad_get_xy(&last_x, &last_y);
      /*Set the coordinates (if released use the last pressed coordinates)*/
      data->point.x = touchX;
      data->point.y = touchY;
      return false; /*Return `false` because we are not buffering and no more data to read*/
    }
    /* Interrupt driven periodic handler */
    static void lv_tick_handler(void)
    {
    lv_tick_inc(LVGL_TICK_PERIOD);
    }

/* 
Command Keyboard
*/
static void cb_keyboard (lv_obj_t * obj, lv_event_t event){
  lv_obj_t * ta = lv_kb_get_ta(obj);
  const char * txt = lv_btnm_get_active_btn_text(obj);
  if (event == LV_EVENT_CLICKED){
    if(txt != NULL) {
        if (strcmp(txt, "<") == 0){
        lv_ta_del_char(ta);
        beep();
        }else if (strcmp(txt, "OK") == 0){
            strcpy(&GCode[strlen(GCode)], lv_ta_get_text(ta));
            
            gc_execute_line(GCode, CLIENT_ALL);
            
            terminal_add(GCode);
            terminal_add("\n");

            grbl_sendf(CLIENT_ALL,GCode);
            grbl_sendf(CLIENT_ALL,"\n");
            
            beep();
            lv_ta_set_text(ta,"");

            protocol_auto_cycle_start();
            return;
        }else{
          lv_ta_add_text(ta,txt);
          beep();
        }
    }}
    if (event == LV_EVENT_LONG_PRESSED){
    if(txt != NULL) {
        if (strcmp(txt, "<") == 0){
         lv_ta_set_text(ta,"");
         beep();
        }
    }
    }
    }

/* 
SPIFFS File utilities
*/
void listDir(fs::FS &fs, const char * dirname, uint8_t levels, const char * &FileNames){
    static char buf[64]= {""};
    File root = fs.open(dirname);
    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            //Serial.println(file.name());
        } else {
            strcpy (&buf[strlen(buf)], file.name());
            strcpy (&buf[strlen(buf)],"\n");
        }
        file = root.openNextFile();
    }
    FileNames = (buf);
    }

void readFile(fs::FS &fs, const char * path){
    Serial.printf("Reading file: %s\r\n", path);

    File file = fs.open(path);
    if(!file || file.isDirectory()){
        Serial.println("- failed to open file for reading");
        return;
    }

    Serial.println("- read from file:");
    while(file.available()){
        Serial.write(file.read());
    }
    }

void writeFile(fs::FS &fs, const char * path, const char * message){
    Serial.printf("Writing file: %s\r\n", path);

    File file = fs.open(path, FILE_WRITE);
    if(!file){
        Serial.println("- failed to open file for writing");
        return;
    }
    if(file.print(message)){
        Serial.println("- file written");
    } else {
        Serial.println("- write failed");
    }
    }

void appendFile(fs::FS &fs, const char * path, const char * message){
    Serial.printf("Appending to file: %s\r\n", path);

    File file = fs.open(path, FILE_APPEND);
    if(!file){
        Serial.println("- failed to open file for appending");
        return;
    }
    if(file.print(message)){
        Serial.println("- message appended");
    } else {
        Serial.println("- append failed");
    }
    }

void renameFile(fs::FS &fs, const char * path1, const char * path2){
    Serial.printf("Renaming file %s to %s\r\n", path1, path2);
    if (fs.rename(path1, path2)) {
        Serial.println("- file renamed");
    } else {
        Serial.println("- rename failed");
    }
    }

void deleteFile(fs::FS &fs, const char * path){
    Serial.printf("Deleting file: %s\r\n", path);
    if(fs.remove(path)){
        Serial.println("- file deleted");
    } else {
        Serial.println("- delete failed");
    }
    }

/* 
WIFI Button
*/
static void btnWIFI_event_cb(lv_obj_t * obj, lv_event_t event){
    if (event == LV_EVENT_PRESSED){
        // start wifi in  STA mode
        Serial.print("Station mode");
        //wifi_config.begin();                
    }
    else if  (event == LV_EVENT_LONG_PRESSED){
        //start wifi in  AP mode
        Serial.print("Access Point  mode");
        //wifi_config.begin();
    }
    }

//static void btnBT_event_cb(lv_obj_t * obj, lv_event_t event){
    // if (event == LV_EVENT_PRESSED){
    //              bt_config.begin();
                 
    //           }           
//    }

/*
 File to Print
*/
static char *fileToPrint;
static void ddFileNamelistevent_handler(lv_obj_t * obj, lv_event_t event){
    static char ddbuf[32]= " ";
            if (event == LV_EVENT_VALUE_CHANGED){
                beep();
                lv_ddlist_get_selected_str(obj  ,ddbuf, sizeof(ddbuf));
                grbl_sendf(CLIENT_ALL, ddbuf );
                grbl_sendf(CLIENT_ALL, " selected.\n" );
                terminal_add(ddbuf);
                fileToPrint = ddbuf;
            }
    }


/* 
Travel Distance dropdown
*/
char travelDist[]= {"10"};
static void ddDistListevent_handler(lv_obj_t * obj, lv_event_t event){
    static char ddbuf[8]= " ";
            if (event == LV_EVENT_VALUE_CHANGED){
                beep();
                lv_ddlist_get_selected_str(obj  ,ddbuf, sizeof(ddbuf));

                strcpy (&travelDist[strlen(travelDist)], ddbuf);

                grbl_sendf(CLIENT_ALL, "Travel distance set to " );
                grbl_sendf(CLIENT_ALL, &travelDist[strlen(travelDist)]);
                grbl_sendf(CLIENT_ALL, " mm. \n" );
                terminal_add("Travel distance set to " );
                terminal_add(travelDist);
                terminal_add(" mm \n");

                
            }
    }  



/*
 Feed Rate
*/

char  FR[]={"500"};
static void sliderFR_event_cb(lv_obj_t * slider, lv_event_t event){
    static int sliderFR;
    char fValue[8];
     if (event == LV_EVENT_VALUE_CHANGED){

         sliderFR = lv_slider_get_value(slider);
        
         dtostrf(sliderFR, 4, 0, fValue);

         strcpy (&FR[strlen(FR)], fValue);

         grbl_sendf(CLIENT_ALL, "Feedrate: " );
         grbl_sendf(CLIENT_ALL,FR );
         grbl_sendf(CLIENT_ALL, " \n" );
         terminal_add(FR);
        
   }}
/*
 Jog Control
*/
static void cb_JogControl(lv_obj_t * btnm, lv_event_t event){
int btnIdx = lv_btnm_get_active_btn(btnm);
const char * txt = lv_btnm_get_active_btn_text(btnm);
  if (event == LV_EVENT_CLICKED){
    if(txt != NULL) {
      switch (btnIdx)
      {
        case 0:
          
          break;
        case 1:
            strcpy (&GCode[strlen(GCode)], "G90G21Y");
            strcat (&GCode[strlen(GCode)], travelDist);
            strcat (&GCode[strlen(GCode)], "F");
            strcat (&GCode[strlen(GCode)],FR);
            //strcat (&GCode[strlen(GCode)],"]");
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            grbl_sendf(CLIENT_ALL, GCode);
            terminal_add(GCode);
            break;
        case 2:
            strcpy (&GCode[strlen(GCode)], "G90G21Z");
            strcat (&GCode[strlen(GCode)], travelDist);
            strcat (&GCode[strlen(GCode)], "F");
            strcat (&GCode[strlen(GCode)],FR);
            //strcat (&GCode[strlen(GCode)],"]");
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            grbl_sendf(CLIENT_ALL, GCode);
            terminal_add(GCode);
            break;
        case 3:
            strcpy (&GCode[strlen(GCode)], "G90G21X-");
            strcat (&GCode[strlen(GCode)], travelDist);
            strcat (&GCode[strlen(GCode)], "F");
            strcat (&GCode[strlen(GCode)],FR);
            //strcat (&GCode[strlen(GCode)],"]");
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            grbl_sendf(CLIENT_ALL, GCode);
            terminal_add(GCode);

            break;
        case 4:
            //report_status_message(gc_execute_line("$HZ0", CLIENT_ALL),CLIENT_ALL);
            strcpy (&GCode[strlen(GCode)], "$HX0");
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            strcpy (&GCode[strlen(GCode)], "$HY0");
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            grbl_sendf(CLIENT_ALL, "Home X&Y");
            grbl_sendf(CLIENT_ALL, "\n");
            terminal_add("Home X&Y\n");
            break;
        case 5:
            strcpy (&GCode[strlen(GCode)], "G90G21X");
            strcat (&GCode[strlen(GCode)], travelDist);
            strcat (&GCode[strlen(GCode)], "F");
            strcat (&GCode[strlen(GCode)],FR);
            //strcat (&GCode[strlen(GCode)],"]");
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            grbl_sendf(CLIENT_ALL, GCode);
            terminal_add(GCode);

            break;
        case 6:
          
          break;
        case 7:
            strcpy (&GCode[strlen(GCode)], "G90G21Y-");
            strcat (&GCode[strlen(GCode)], travelDist);
            strcat (&GCode[strlen(GCode)], "F");
            strcat (&GCode[strlen(GCode)],FR);
            //strcat (&GCode[strlen(GCode)],"]");
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            grbl_sendf(CLIENT_ALL, GCode);
            terminal_add(GCode);
            break;
        case 8:
            strcpy (&GCode[strlen(GCode)], "G90G21Z-");
            strcat (&GCode[strlen(GCode)], travelDist);
            strcat (&GCode[strlen(GCode)], "F");
            strcat (&GCode[strlen(GCode)],FR);
            report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
            grbl_sendf(CLIENT_ALL, GCode);
            terminal_add(GCode);
            break;
      default:
          break;
      }
      //clear serial buffer
      
    }}
}

/*
 Job Control 
*/
static void cb_JobControl(lv_obj_t * btnm, lv_event_t event){
int btnIdx = lv_btnm_get_active_btn(btnm);
  if (event == LV_EVENT_CLICKED){
      switch (btnIdx)
      {
        case 0:
          strcpy (&GCode[strlen(GCode)], "$X");
          report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
          terminal_add(GCode);
          grbl_sendf(CLIENT_ALL, "Locked");
          grbl_sendf(CLIENT_ALL, "\n");
          //empty line buffer


          break; 
        case 1:
          strcpy (&GCode[strlen(GCode)], "!");
          report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
          terminal_add(GCode);
          grbl_sendf(CLIENT_ALL, "Paused");
          grbl_sendf(CLIENT_ALL, "\n");
          break;
        case 2:
          strcpy (&GCode[strlen(GCode)], "0x18");
          report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
          terminal_add(GCode);
          grbl_sendf(CLIENT_ALL, "Reset");
          grbl_sendf(CLIENT_ALL, "\n");
          break;
        case 3:
          strcpy (&GCode[strlen(GCode)], "file test.gcode");
          report_status_message(gc_execute_line(GCode, CLIENT_ALL),CLIENT_ALL);
          terminal_add(GCode);
          grbl_sendf(CLIENT_ALL, "Go");
          grbl_sendf(CLIENT_ALL, "\n");
          break;
      default:
          break;
      }
    }
}
//mc_reset();
/*
 GUI Setup
*/
static void setupGUI() {

    pinMode(buzzerPin, OUTPUT);

    SPIFFS.begin();
    const char *FileNames;
    listDir(SPIFFS, "/", 0, FileNames);

  lv_init();
  #if USE_LV_LOG != 0
    lv_log_register_print(my_print); /* register print function for debugging */
  #endif

  tft.begin(); /* TFT init */
  tft.setRotation(1);
  
  lv_theme_t * th = lv_theme_night_init(45, NULL);
  lv_theme_set_current(th);
 
  lv_disp_buf_init(&disp_buf, buf, NULL, LV_HOR_RES_MAX * 10);
  /*Initialize the display*/
  lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.buffer = &disp_buf;
  lv_disp_drv_register(&disp_drv);
  
  /*Initialize the touch pad*/
  lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);             /*Descriptor of a input device driver*/
  indev_drv.type = LV_INDEV_TYPE_POINTER;    /*Touch pad is a pointer-like device*/
  indev_drv.read_cb = my_touchpad_read;      /*Set your driver function*/
  lv_indev_drv_register(&indev_drv);         /*Finally register the driver*/

  /*Initialize the graphics library's tick*/
  tick.attach_ms(LVGL_TICK_PERIOD, lv_tick_handler);

  lv_obj_t * scr = lv_cont_create(NULL, NULL);
  lv_disp_load_scr(scr);
  terminal_create();

    static lv_obj_t * imgBG;
    LV_IMG_DECLARE(tnbStripes);
    imgBG = lv_img_create(scr, NULL);
    lv_img_set_src(imgBG, &tnbStripes);
    lv_obj_align(imgBG, NULL,LV_ALIGN_CENTER, 0, 140);
    static lv_obj_t * imgBGTop;
    imgBGTop = lv_img_create(scr, NULL);
    lv_img_set_src(imgBGTop, &tnbStripes);
    lv_obj_align(imgBGTop, NULL,LV_ALIGN_CENTER, 0, -140);
    
 


    //keyboard to send gcode commands ===================================================================
    static const char * btnmKB_map[]= {"1","2","3","\n", 
                                       "4","5","6","\n", 
                                       "7","8","9","\n", 
                                       "<","0",".","\n",
                                       "-","$","+","\n",
                                       " ","\n",
                                       "X","Y","Z","\n", 
                                       "G","F","OK",""};
    lv_obj_t *btnmKB = lv_kb_create(scr,NULL);
    lv_btnm_set_map (btnmKB, btnmKB_map);
    lv_kb_set_cursor_manage(btnmKB, true);
    lv_obj_set_size(btnmKB,100,250);
    //lv_obj_set_style( btnmKB , &fjr_style01);
    lv_obj_align(btnmKB, NULL, LV_ALIGN_CENTER, -180,-22);
    lv_obj_set_event_cb (btnmKB, cb_keyboard);
    //text area for gcode command ==============================
    lv_obj_t *taGC = lv_ta_create(scr, NULL);
    lv_obj_set_size(taGC, 250,28);
    lv_obj_align(taGC, NULL, LV_ALIGN_CENTER, 0, -132);
    lv_ta_set_one_line(taGC, true);
    lv_ta_set_max_length(taGC,32);
    lv_kb_set_ta(btnmKB,taGC);
    lv_ta_set_cursor_type(taGC, LV_CURSOR_UNDERLINE);
    lv_ta_set_text(taGC, ""); 

    //jog control ========================================================================================
    static const char * btnm_map[]= {"A+", "Y+","Z+", "\n", 
                                     "-X", LV_SYMBOL_HOME, "X+","\n",
                                     "A-", "-Y", "Z-", ""};
    lv_obj_t *btnmJogControl = lv_btnm_create(scr,NULL);
    lv_btnm_set_map (btnmJogControl, btnm_map);
    lv_btnm_set_btn_ctrl(btnmJogControl,0,LV_BTNM_CTRL_HIDDEN);
    lv_btnm_set_btn_ctrl(btnmJogControl,6,LV_BTNM_CTRL_HIDDEN);
    lv_obj_set_size(btnmJogControl,105,100);
    lv_obj_align(btnmJogControl, NULL, LV_ALIGN_CENTER, -70, -53);
    lv_obj_set_event_cb (btnmJogControl, cb_JogControl);
 
    // Labels for bluetooth and wifi==========================================
    lv_obj_t *btnWIFI = lv_btn_create(scr, NULL);
    lv_obj_set_size(btnWIFI,30,30);
    lv_obj_align(btnWIFI, NULL, LV_ALIGN_CENTER, 220,-135); 
    lv_obj_t *labelWIFI = lv_label_create (btnWIFI, NULL);
    lv_label_set_text (labelWIFI, LV_SYMBOL_WIFI);
    lv_obj_set_event_cb (btnWIFI, btnWIFI_event_cb);
       
    // lv_obj_t *btnBT = lv_btn_create(scr, NULL);
    // lv_obj_set_size(btnBT,30,30);
    // lv_obj_align(btnBT, NULL, LV_ALIGN_CENTER, 160,-135); 
    // lv_obj_t *labelBT = lv_label_create (btnBT, NULL);
    // lv_label_set_text (labelBT, LV_SYMBOL_BLUETOOTH);
    // lv_obj_set_event_cb (btnBT, btnBT_event_cb);
    
    // Labels for X Y Z=============================================================
    lv_obj_t *labelX = lv_label_create (scr, NULL);
    lv_label_set_text (labelX, "X : ");
    lv_obj_align(labelX, NULL, LV_ALIGN_IN_LEFT_MID, 370,-100); 
     
    lv_obj_t *labelY = lv_label_create (scr, NULL);
    lv_label_set_text (labelY, "Y : ");
    lv_obj_align(labelY, NULL, LV_ALIGN_IN_LEFT_MID, 370,-70);  

    lv_obj_t *labelZ = lv_label_create (scr, NULL);
    lv_label_set_text (labelZ, "Z : ");
    lv_obj_align(labelZ, NULL, LV_ALIGN_IN_LEFT_MID, 370,-40); 

    // Labels for X Y Z Data=============================================================
    static lv_style_t fjr_style01;
    lv_style_copy (&fjr_style01, &lv_style_pretty_color);
    fjr_style01.text.font = &lv_font_roboto_28;

    lv_obj_t *labelXData = lv_label_create (scr, NULL);
    lv_label_set_text (labelXData, "00.00");
    lv_label_set_style(labelXData,LV_LABEL_STYLE_MAIN , &fjr_style01);
    lv_obj_align(labelXData, NULL, LV_ALIGN_IN_LEFT_MID, 400,-100); 
     
    lv_obj_t *labelYData = lv_label_create (scr, NULL);
    lv_label_set_text (labelYData, "00.00");
    lv_label_set_style(labelYData,LV_LABEL_STYLE_MAIN , &fjr_style01);
    lv_obj_align(labelYData, NULL, LV_ALIGN_IN_LEFT_MID, 400,-70);  

    lv_obj_t *labelZData = lv_label_create (scr, NULL);
    lv_label_set_text (labelZData, "00.00");
    lv_label_set_style(labelZData,LV_LABEL_STYLE_MAIN , &fjr_style01);
    lv_obj_align(labelZData, NULL, LV_ALIGN_IN_LEFT_MID, 400,-40); 
    
    // Labels for TRAVEL AND FEEDRATE===============================
    // lv_obj_t *labelTravel = lv_label_create (scr, NULL);
    // lv_label_set_text (labelTravel, "Travel : \n in mm");
    // lv_obj_align(labelTravel, NULL, LV_ALIGN_IN_LEFT_MID, 225,-80); 
     
    lv_obj_t *labelFeedrate = lv_label_create (scr, NULL);
    lv_label_set_text (labelFeedrate, "Feedrate: ");
    lv_obj_align(labelFeedrate, NULL, LV_ALIGN_IN_LEFT_MID, 225,-40);  

    //controls for sending files from spiffs to machine
    static const char * btnmJob_map[]= { LV_SYMBOL_POWER , "\n",LV_SYMBOL_PAUSE, LV_SYMBOL_REFRESH, "\n",LV_SYMBOL_PLAY ,""};
    lv_obj_t *btnmJobControl = lv_btnm_create(scr,NULL);
        lv_btnm_set_map (btnmJobControl, btnmJob_map);
        lv_obj_set_size(btnmJobControl,100,100);
        lv_obj_align(btnmJobControl, NULL, LV_ALIGN_CENTER, 180,100);  
        lv_obj_set_event_cb (btnmJobControl, cb_JobControl);
    
    //Feedrate Slider
    lv_obj_t * sliderFR = lv_slider_create(scr, NULL);
        lv_obj_set_size(sliderFR,200,30);
        lv_obj_align(sliderFR, NULL, LV_ALIGN_CENTER, 110,-15); 
        lv_slider_set_range(sliderFR,10,2000);
        lv_obj_set_event_cb(sliderFR, sliderFR_event_cb); 
        
    lv_obj_t * sliderFR_DataLabel = lv_label_create(scr,NULL);
        lv_label_set_text (sliderFR_DataLabel,FR);
        lv_obj_align(sliderFR_DataLabel, NULL, LV_ALIGN_IN_LEFT_MID, 320,-40);  

    //distance drop down list ============================================================================    
    lv_obj_t * ddDistlist = lv_ddlist_create(scr, NULL);
    lv_ddlist_set_options(ddDistlist,"50\n10\n5.0\n1.0\n0.1\n0.01");
    lv_ddlist_set_fix_width(ddDistlist, 80);
    lv_ddlist_set_fix_height(ddDistlist, 0);
    lv_ddlist_set_draw_arrow(ddDistlist, true);
    lv_obj_align(ddDistlist, NULL, LV_ALIGN_CENTER, 80, -80);
    lv_obj_set_event_cb(ddDistlist, ddDistListevent_handler);
  
    //Create a dropdown list area for filename =================================================================
    lv_obj_t * ddFileNamelist = lv_ddlist_create(scr, NULL);
    lv_ddlist_set_options(ddFileNamelist, FileNames );
    lv_ddlist_set_fix_width(ddFileNamelist, 355);
    lv_ddlist_set_fix_height(ddFileNamelist,0);
    lv_ddlist_set_draw_arrow(ddFileNamelist, true);
    lv_obj_set_auto_realign(ddFileNamelist, true);
    lv_obj_align(ddFileNamelist, NULL, LV_ALIGN_IN_BOTTOM_MID ,  -52, -10);
    lv_obj_set_event_cb(ddFileNamelist, ddFileNamelistevent_handler);

    }
