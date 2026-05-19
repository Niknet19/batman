#!/bin/bash

# Загрузка модуля
#sudo modprobe batman-adv
#modinfo batman-adv
sudo batctl routing_algo BATMAN_V
# Создание mesh интерфейса
sudo batctl meshif bat0 create

# Остановка менеджеров, которые мешают
sudo systemctl stop NetworkManager 2>/dev/null
sudo systemctl stop wpa_supplicant 2>/dev/null

# Снятие блокировки rfkill
#sudo rfkill unblock wifi

# Настройка wlan0
sudo ip link set wlan0 down
sudo ip addr flush dev wlan0
sudo iw dev wlan0 set type ibss
sudo iw dev wlan0 ibss join MyMesh 2412
sudo ip link set wlan0 up

# Добавление wlan0 в mesh
sudo batctl meshif bat0 interface add wlan0

# Настройка bat0
sudo ip link set bat0 up
sudo ip addr add 10.0.0.1/24 dev bat0

echo "Mesh сеть запущена. IP адрес: 10.0.0.1"
