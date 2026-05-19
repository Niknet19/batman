#!/bin/bash

# Отвязываем интерфейс от mesh
sudo batctl meshif bat0 interface del wlan0 2>/dev/null
sudo ip link set dev wlan0 nomaster 2>/dev/null

# Удаляем mesh интерфейс
sudo ip link set bat0 down 2>/dev/null
sudo ip link delete bat0 2>/dev/null

# Возвращаем wlan0 в обычный режим
sudo ip link set wlan0 down
sudo ip addr flush dev wlan0
sudo iw dev wlan0 set type managed
sudo iw dev wlan0 ibss leave 2>/dev/null

# Поднимаем wlan0
sudo ip link set wlan0 up

# Запускаем менеджеры обратно
sudo systemctl start NetworkManager 2>/dev/null
sudo systemctl start wpa_supplicant 2>/dev/null

echo "Mesh сеть отключена. Wi-Fi в обычном режиме."
