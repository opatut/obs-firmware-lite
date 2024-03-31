#include <Arduino.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <utils/button.h>

#include <PacketSerial.h>

PacketSerial packetSerial;

#include "openbikesensor.pb.h"

// Button config
const int PUSHBUTTON_PIN = 2;
int numButtonReleased = 0;
Button button(PUSHBUTTON_PIN);

uint8_t pb_buffer[1024];
pb_ostream_t pb_ostream;

bool _write_string(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    String &str = *((String *)(*arg));

    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }

    return pb_encode_string(stream, (uint8_t *)str.c_str(), str.length());
}
void write_string(pb_callback_t &target, String &str)
{
    target.arg = &str;
    target.funcs.encode = &_write_string;
}

void send_text_message(String message, openbikesensor_TextMessage_Type type = openbikesensor_TextMessage_Type_INFO)
{
    // create the time
    openbikesensor_Time cpu_time = openbikesensor_Time_init_zero;
    uint32_t us = micros();
    cpu_time.seconds = us / 1000000;
    cpu_time.nanoseconds = (us % 1000000) * 1000;

    // create the text message
    openbikesensor_TextMessage msg = openbikesensor_TextMessage_init_zero;
    msg.type = type;
    write_string(msg.text, message);

    // create the event
    openbikesensor_Event event = openbikesensor_Event_init_zero;
    event.time_count = 1;
    event.time[0] = cpu_time;
    event.content.text_message = msg;
    event.which_content = openbikesensor_Event_text_message_tag;

    // write out
    pb_ostream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
    pb_encode(&pb_ostream, &openbikesensor_Event_msg, &event);
    packetSerial.send(pb_buffer, pb_ostream.bytes_written);
}

void send_distance_measurement(uint32_t source_id, float distance, uint64_t time_of_flight)
{
    // create the time
    openbikesensor_Time cpu_time = openbikesensor_Time_init_zero;
    uint32_t us = micros();
    cpu_time.seconds = us / 1000000;
    cpu_time.nanoseconds = (us % 1000000) * 1000;

    // create the distance measurement
    openbikesensor_DistanceMeasurement distance_measurement = openbikesensor_DistanceMeasurement_init_zero;
    distance_measurement.source_id = source_id;
    distance_measurement.distance = distance;
    distance_measurement.time_of_flight = time_of_flight;

    // create the event
    openbikesensor_Event event = openbikesensor_Event_init_zero;
    event.time_count = 1;
    event.time[0] = cpu_time;
    event.content.distance_measurement = distance_measurement;
    event.which_content = openbikesensor_Event_distance_measurement_tag;

    // write out
    pb_ostream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
    pb_encode(&pb_ostream, &openbikesensor_Event_msg, &event);
    packetSerial.send(pb_buffer, pb_ostream.bytes_written);
}

void send_button_press()
{
    // create the time
    openbikesensor_Time cpu_time = openbikesensor_Time_init_zero;
    uint32_t us = micros();
    cpu_time.seconds = us / 1000000;
    cpu_time.nanoseconds = (us % 1000000) * 1000;

    openbikesensor_UserInput user_input = openbikesensor_UserInput_init_zero;
    user_input.type = openbikesensor_UserInput_Type_OVERTAKER;
    user_input.direction = openbikesensor_UserInput_Direction_LEFT;
    user_input.timing = openbikesensor_UserInput_Timing_IMMEDIATE;

    // create the event
    openbikesensor_Event event = openbikesensor_Event_init_zero;
    event.time_count = 1;
    event.time[0] = cpu_time;
    event.content.user_input = user_input;
    event.which_content = openbikesensor_Event_user_input_tag;

    // write out
    pb_ostream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
    pb_encode(&pb_ostream, &openbikesensor_Event_msg, &event);
    packetSerial.send(pb_buffer, pb_ostream.bytes_written);
}

void send_heartbeat()
{
    // create the time
    openbikesensor_Time cpu_time = openbikesensor_Time_init_zero;
    uint32_t us = micros();
    cpu_time.seconds = us / 1000000;
    cpu_time.nanoseconds = (us % 1000000) * 1000;

    // create the event
    openbikesensor_Event event = openbikesensor_Event_init_zero;
    event.time_count = 1;
    event.time[0] = cpu_time;

    // write out
    pb_ostream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
    pb_encode(&pb_ostream, &openbikesensor_Event_msg, &event);
    packetSerial.send(pb_buffer, pb_ostream.bytes_written);
}

class SensorMeasurement
{
public:
    uint32_t start;
    uint32_t tof; // microseconds
    bool timeout = false;

    const double get_distance(const double temperature = 19.307) const
    {
        // temperature in degree celsius, returns meters

        // formula from https://www.engineeringtoolbox.com/air-speed-sound-d_603.html
        double speedOfSound = 20.05 * sqrt(273.16 + temperature);

        // factor 2.0 because the sound travels the distance twice
        return speedOfSound * tof / 1000000.0 / 2.0;
    }
};

class Sensor
{
public:
    Sensor(uint8_t source_id_, uint8_t _trigger_pin, uint8_t _echo_pin) : source_id(source_id_),
                                                                          trigger_pin(_trigger_pin),
                                                                          echo_pin(_echo_pin) {}

    void begin(void interrupt_echo())
    {
        pinMode(echo_pin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(echo_pin), interrupt_echo, CHANGE);
        pinMode(trigger_pin, OUTPUT);
    }

    void echo()
    {
        if (digitalRead(echo_pin))
        {
            start = micros();
        }
        else
        {
            end = micros();
        }
    }

    void update(bool master, Sensor slave)
    {
        uint32_t now = micros();

        // Read response
        if (start > 0 && end > 0)
        {
            uint32_t tof = end - start;
            measurement.start = start;
            measurement.tof = end - start;
            measurement.timeout = tof > no_response_threshold; // threshold is 38ms
            has_new_measurement = true;

            // Change state
            trigger_at = max(start + interval, now + min_delay);
            triggered = 0;
            start = 0;
            end = 0;
            timeout_at = 0;

            return;
        }

        // Trigger
        if (trigger_at > 0 && now > trigger_at && master)
        {
            // Start the timeout, stop triggering

            // Pull trigger line high for >10us

            trigger_at = 0;
            triggered = now;
            timeout_at = now + timeout;
            bool slave_ready = (slave.trigger_at>0) && (now>slave.trigger_at);
            if (slave_ready)
            {
                slave.trigger_at = 0;
                slave.triggered = now;
                slave.timeout_at = timeout_at - min_delay;
                digitalWrite(slave.trigger_pin, HIGH);
            } 
            digitalWrite(trigger_pin, HIGH);

            delayMicroseconds(20);
            digitalWrite(trigger_pin, LOW);
            if (slave_ready)
            {
                digitalWrite(slave.trigger_pin, LOW);
            }
        }

        // Timeout
        if (timeout_at > 0 && now > timeout_at)
        {
            trigger_at = max(triggered + interval, now + min_delay);
            triggered = 0;
            start = 0;
            end = 0;
            timeout_at = 0;

            // Put out info about timeout
            measurement.timeout = true;
            has_new_measurement = true;
        }
    }

    bool measuring;

    uint8_t source_id;
    uint8_t trigger_pin;
    uint8_t echo_pin;
    uint32_t start = 0;
    uint32_t triggered = 0;
    uint32_t end = 0;
    uint32_t trigger_at = 1;
    uint32_t timeout_at = 0;

    SensorMeasurement measurement;
    bool has_new_measurement;

    uint32_t interval = 40000;              // target 40ms interval
    uint32_t min_delay = 5000;              // min delay 5ms between echo and next trigger
    uint32_t no_response_threshold = 35000; // 38ms according to datasheet
    uint32_t timeout = 50000;               // after
};

Sensor sensors[] = {
    Sensor(1, 15, 4),
    Sensor(2, 25, 26),
};
uint8_t sensors_length = 2;

void IRAM_ATTR interrupt_sensor0()
{
    sensors[0].echo();
}

void IRAM_ATTR interrupt_sensor1()
{
    sensors[1].echo();
}

class Timer
{
public:
    Timer(uint32_t delay_) : delay(delay_) {}

    void start()
    {
        trigger_at = millis() + delay;
    }

    bool check()
    {
        if (trigger_at <= millis())
        {
            trigger_at = 0;
            return true;
        }
        return false;
    }

private:
    uint32_t trigger_at = 0;
    uint32_t delay;
};

Timer heartbeat(1000);

void setup()
{
    packetSerial.begin(115200);

    sensors[0].begin(interrupt_sensor0);
    sensors[1].begin(interrupt_sensor1);

    heartbeat.start();
}

void loop()
{
    // update all sensors, triggering them as required and processing returned
    // interrupts
    for (uint8_t i = 0; i < sensors_length; i++)
    {
        sensors[i].update(i==0, sensors[1]);
    }

    if (heartbeat.check())
    {
        send_heartbeat();
        heartbeat.start();
    }

    // read all measurements and send them via serial
    for (uint8_t i = 0; i < sensors_length; i++)
    {
        Sensor &sensor = sensors[i];

        if (sensor.has_new_measurement)
        {
            SensorMeasurement const &measurement = sensor.measurement;

            double distance = 99.0;
            uint32_t tof = 10000;

            if (!measurement.timeout)
            {
                distance = measurement.get_distance();
                tof = measurement.tof * 1000; // microseconds to nanoseconds
            }

            send_distance_measurement(sensor.source_id, distance, tof);
            sensor.has_new_measurement = false;
        }
    }

    button.handle();

    if (button.gotPressed())
    {
        send_button_press();
        // send_text_message("Button got pressed");
    }

    // read and receive packets from serial input
    packetSerial.update();
}
