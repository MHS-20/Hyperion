cd module
make
sudo rmmod hyperion
sudo insmod hyperion.ko

cd ..
cd user
make
sudo ./user/hyperion-user
