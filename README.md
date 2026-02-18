# 🌱 Smart Irrigation System (ESP32)

An IoT-based Smart Irrigation System built using **ESP32**, designed to automate plant watering based on real-time environmental conditions.

---

## 🚀 Features

✅ Soil Moisture Monitoring  
✅ Automatic Pump Control  
✅ Rain Detection Safety  
✅ Temperature & Humidity Tracking (DHT22)  
✅ Water Flow Measurement (mL)  
✅ OpenWeather API Integration  
✅ Blynk Mobile App Control  

---

## 🛠 Hardware Used

- ESP32 Dev Module
- Capacitive Soil Moisture Sensor
- DHT22 Temperature & Humidity Sensor
- Rain Sensor Module
- Water Flow Sensor (YF-S201)
- 5V Relay Module
- 5V Water Pump
- External Power Supply

---

## ⚙️ System Logic

- Pump turns **ON** when soil is dry
- Pump turns **OFF** when soil reaches threshold
- Pump turns **OFF immediately** if rain detected
- Total water irrigated measured using flow sensor
- Data sent to **Blynk App**

---

## 📱 Blynk Datastreams

| Parameter | Virtual Pin |
|----------|-------------|
| Soil Moisture | V1 |
| Temperature | V2 |
| Humidity | V3 |
| Auto Mode | V10 |
| Pump Control | V11 |
| Rain Status | V12 |
| Total Water Irrigated | V20 |

---

## 🌦 API Integration

Weather data retrieved from:

**OpenWeather API**

Used for:
- Current Weather
- Rain Forecast Check

---

## 📊 Output

- Soil Moisture (%)
- Temperature (°C)
- Humidity (%)
- Rain Detection
- Pump Status
- Total Water Irrigated (mL)

---

## 🎯 Purpose

This project demonstrates:

- IoT automation
- Sensor integration
- Smart water management
- Sustainable irrigation

---

## 👨‍💻 Developed By

**Nikhil SP**

---

## 📌 Future Improvements

- AI/ML irrigation prediction
- Cloud data logging
- Multi-zone irrigation
- Solar-powered system

---
