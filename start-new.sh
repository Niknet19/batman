#!/bin/bash
sleep 15

# Конфигурация (из файлов или по умолчанию)
ROLE=$(cat /etc/mesh/role 2>/dev/null || echo "bridge")
CHANNEL=$(cat /etc/mesh/channel 2>/dev/null || echo "8")
ESSID=$(cat /etc/mesh/essid 2>/dev/null || echo "MyMesh")
FREQ=$(cat /etc/mesh/freq 2>/dev/null || echo "2447")
IP_ADDR=$(cat /etc/mesh/ip 2>/dev/null || echo "10.0.0.3")
GW=$(cat /etc/mesh/gateway 2>/dev/null || echo "10.0.0.1")



# Разблокировка Wi-Fi и остановка мешающих служб
sudo rfkill unblock wifi
sudo systemctl stop hostapd 2>/dev/null || true
sudo systemctl disable NetworkManager 2>/dev/null || true
sudo systemctl stop NetworkManager 2>/dev/null || true
sudo systemctl stop wpa_supplicant 2>/dev/null || true
sudo pkill -9 wpa_supplicant 2>/dev/null || true

# Загружаем batman-adv
sudo modprobe batman-adv
sleep 2

# Настройка wlan0 в режим IBSS на НУЖНОЙ ЧАСТОТЕ
sudo ip link set wlan0 down
sudo iwconfig wlan0 mode ad-hoc
sudo iwconfig wlan0 channel $CHANNEL
sudo iwconfig wlan0 essid $ESSID
# Явно задаём частоту (для 5 ГГц это критично!)
sudo iw dev wlan0 set freq $FREQ 2>/dev/null || sudo iwconfig wlan0 freq $FREQ
sudo ip link set wlan0 up
sleep 3

# Проверяем, что частота применилась
ACTUAL_FREQ=$(iw dev wlan0 info | grep freq | awk '{print $2}')
echo "Запрошенная частота: $FREQ МГц, фактическая: $ACTUAL_FREQ МГц"

# Создаём bat0 и добавляем wlan0
sudo batctl meshif bat0 create 2>/dev/null || sudo ip link add name bat0 type batadv
sudo batctl meshif bat0 interface add wlan0 2>/dev/null || sudo ip link set dev wlan0 master bat0

# Устанавливаем BATMAN_V
sudo batctl -m bat0 routing_algo BATMAN_V
echo "Алгоритм маршрутизации: $(sudo batctl -m bat0 routing_algo)"

# Настройка bat0
sudo ip link set bat0 up
sudo ifconfig bat0 mtu 1468

# Настройка IP на bat0 (без br0!)
sudo ip addr flush dev bat0 2>/dev/null || true
sudo ip addr add $IP_ADDR/24 dev bat0
sudo ip route add default via $GW dev bat0 2>/dev/null || true

# Режим шлюза
sudo batctl -m bat0 gw_mode client 2>/dev/null || true

# Показываем результат
echo ""
echo "=== Mesh-сеть запущена ==="
echo "Интерфейс bat0:"
ip addr show bat0 | grep inet
echo ""
echo "Интерфейсы в mesh:"
sudo batctl -m bat0 interface
echo ""
echo "Частота wlan0:"
iw dev wlan0 info | grep -E "channel|freq"
echo ""
echo "Соседи (originators):"
sudo batctl -m bat0 o
echo ""
echo "Логи batman:"
sudo batctl -m bat0 log 2>/dev/null || echo "Логи недоступны"