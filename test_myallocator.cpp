
#include"myallocator.h"
#include<vector>

using namespace std;

int main() {
	vector<int, myallocator<int>> vec;
	for (int i = 0; i < 100; i++) 
	{
		vec.push_back(rand()%100);
	}
	for (int val : vec) {
		cout << val <<"    " << endl;
	}
	return 0;
}