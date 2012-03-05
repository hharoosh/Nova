//============================================================================
// Name        : ClassificationEngine.h
// Copyright   : DataSoft Corporation 2011-2012
//	Nova is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
//   Nova is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with Nova.  If not, see <http://www.gnu.org/licenses/>.
// Description : Suspect classification engine
//============================================================================

#include "ClassificationEngine.h"

#include <boost/format.hpp>

using namespace std;
using boost::format;

// Normalization method to use on each feature
// TODO: Make this a configuration var somewhere in Novaconfig.txt?
normalizationType ClassificationEngine::normalization[] = {
		LINEAR_SHIFT, // Don't normalize IP traffic distribution, already between 0 and 1
		LINEAR_SHIFT,
		LOGARITHMIC,
		LOGARITHMIC,
		LOGARITHMIC,
		LOGARITHMIC,
		LOGARITHMIC,
		LOGARITHMIC,
		LOGARITHMIC
};

ClassificationEngine::ClassificationEngine(Logger *logger, NOVAConfiguration *configuration, SuspectHashTable *table)
: logger(logger), globalConfig(configuration), suspects(table)
{
	nPts = 0;
	enabledFeatures = 0;
	featureMask = 0;
	for (uint i = 0; i < sizeof (featureEnabled); i++)
		featureEnabled[i] = false;
}

ClassificationEngine::~ClassificationEngine()
{

}

void ClassificationEngine::SetEnabledFeatures(string enabledFeatureMask)
{
	enabledFeatures = 0;
	featureMask = 0;
	for (uint i = 0; i < DIM; i++)
	{
		if ('1' == enabledFeatureMask.at(i))
		{
			featureEnabled[i] = true;
			featureMask += pow(2, i);
			enabledFeatures++;
		}
		else
		{
			featureEnabled[i] = false;
		}
	}

	sqrtDIM = sqrt(enabledFeatures);
}


void ClassificationEngine::FormKdTree()
{
	delete kdTree;
	//Normalize the data points
	//Foreach data point
	for(uint j = 0;j < enabledFeatures;j++)
	{
		//Foreach feature within the data point
		for(int i=0;i < nPts;i++)
		{
			if(maxFeatureValues[j] != 0)
			{
				normalizedDataPts[i][j] = Normalize(normalization[j], dataPts[i][j], minFeatureValues[j], maxFeatureValues[j]);
			}
			else
			{
				logger->Log(ERROR, (format("File %1% at line %2%: The max value of a feature was 0. Is the training data file corrupt or missing?")%__LINE__%__FILE__).str());
				break;
			}
		}
	}
	kdTree = new ANNkd_tree(					// build search structure
			normalizedDataPts,					// the data points
					nPts,						// number of points
					enabledFeatures);						// dimension of space
	//updateKDTree = false;
}


void ClassificationEngine::Classify(Suspect *suspect)
{
	int k = globalConfig->getK();
	ANNidxArray nnIdx = new ANNidx[k];			// allocate near neigh indices
	ANNdistArray dists = new ANNdist[k];		// allocate near neighbor dists

	kdTree->annkSearch(							// search
			suspect->m_annPoint,					// query point
			k,									// number of near neighbors
			nnIdx,								// nearest neighbors (returned)
			dists,								// distance (returned)
			globalConfig->getEps());								// error bound

	for (int i = 0; i < DIM; i++)
		suspect->m_featureAccuracy[i] = 0;

	suspect->SetHostileNeighbors(0);

	//Determine classification according to weight by distance
	//	.5 + E[(1-Dist) * Class] / 2k (Where Class is -1 or 1)
	//	This will make the classification range from 0 to 1
	double classifyCount = 0;

	for (int i = 0; i < k; i++)
	{
		dists[i] = sqrt(dists[i]);				// unsquare distance

		for (int j = 0; j < DIM; j++)
		{
			if (featureEnabled[j])
			{
				double distance = suspect->m_annPoint[j] - kdTree->thePoints()[nnIdx[i]][j];
				if (distance < 0)
					distance *= -1;

				suspect->m_featureAccuracy[j] += distance;
			}
		}

		if(nnIdx[i] == -1)
		{
			logger->Log(ERROR, (format("File %1% at line %2%: Unable to find a nearest neighbor for Data point %3% Try decreasing the Error bound")
					%__LINE__%__FILE__%i).str());
		}
		else
		{
			//If Hostile
			if(dataPtsWithClass[nnIdx[i]]->m_classification == 1)
			{
				classifyCount += (sqrtDIM - dists[i]);
				suspect->SetHostileNeighbors(suspect->GetHostileNeighbors()+1);
			}
			//If benign
			else if(dataPtsWithClass[nnIdx[i]]->m_classification == 0)
			{
				classifyCount -= (sqrtDIM - dists[i]);
			}
			else
			{
				//error case; Data points must be 0 or 1
				logger->Log(ERROR, (format("File %1% at line %2%: Data point has invalid classification. Should by 0 or 1, but is %3%")
						%__LINE__%__FILE__%dataPtsWithClass[nnIdx[i]]->m_classification).str());

				suspect->SetClassification(-1);
				delete [] nnIdx;							// clean things up
				delete [] dists;
				annClose();
				return;
			}
		}
	}

	for (int j = 0; j < DIM; j++)
				suspect->m_featureAccuracy[j] /= k;


	suspect->SetClassification(.5 + (classifyCount / ((2.0 * (double)k) * sqrtDIM )));

	// Fix for rounding errors caused by double's not being precise enough if DIM is something like 2
	if (suspect->GetClassification() < 0)
		suspect->SetClassification(0);
	else if (suspect->GetClassification() > 1)
		suspect->SetClassification(1);

	if( suspect->GetClassification() > globalConfig->getClassificationThreshold())
	{
		suspect->SetIsHostile(true);
	}
	else
	{
		suspect->SetIsHostile(false);
	}
	delete [] nnIdx;							// clean things up
    delete [] dists;

    annClose();
	suspect->SetNeedsClassificationUpdate(false);
}


void ClassificationEngine::NormalizeDataPoints()
{
	for (SuspectHashTable::iterator it = suspects->begin();it != suspects->end();it++)
	{
		// Used for matching the 0->DIM index with the 0->enabledFeatures index
		int ai = 0;
		for(int i = 0;i < DIM;i++)
		{
			if (featureEnabled[i])
			{
				if(it->second->m_features.m_features[i] > maxFeatureValues[ai])
				{
					//For proper normalization the upper bound for a feature is the max value of the data.
					it->second->m_features.m_features[i] = maxFeatureValues[ai];
				}
				else if (it->second->m_features.m_features[i] < minFeatureValues[ai])
				{
					it->second->m_features.m_features[i] = minFeatureValues[ai];
				}
				ai++;
			}

		}
	}

	//if(updateKDTree) FormKdTree();

	//Normalize the suspect points
	for (SuspectHashTable::iterator it = suspects->begin();it != suspects->end();it++)
	{
		if(it->second->GetNeedsFeatureUpdate())
		{
			if(it->second->m_annPoint == NULL)
				it->second->m_annPoint = annAllocPt(enabledFeatures);

			int ai = 0;
			for(int i = 0;i < DIM;i++)
			{
				if (featureEnabled[i])
				{
					if(maxFeatureValues[ai] != 0)
						it->second->m_annPoint[ai] = Normalize(normalization[i], it->second->m_features.m_features[i], minFeatureValues[ai], maxFeatureValues[ai]);
					else
						logger->Log(ERROR, (format("File %1% at line %2%: Max value for a feature is 0. Normalization failed. Is the training data corrupt or missing?")
								%__LINE__%__FILE__).str());
					ai++;
				}

			}
			it->second->SetNeedsFeatureUpdate(false);
		}
	}
}


void ClassificationEngine::PrintPt(ostream &out, ANNpoint p)
{
	out << "(" << p[0];
	for (uint i = 1;i < enabledFeatures;i++)
	{
		out << ", " << p[i];
	}
	out << ")\n";
}


void ClassificationEngine::LoadDataPointsFromFile(string inFilePath)
{
	ifstream myfile (inFilePath.data());
	string line;

	// Clear max and min values
	for (int i = 0; i < DIM; i++)
		maxFeatureValues[i] = 0;

	for (int i = 0; i < DIM; i++)
		minFeatureValues[i] = 0;

	for (int i = 0; i < DIM; i++)
		meanFeatureValues[i] = 0;

	// Reload the data file
	if (dataPts != NULL)
		annDeallocPts(dataPts);
	if (normalizedDataPts != NULL)
		annDeallocPts(normalizedDataPts);

	dataPtsWithClass.clear();

	//string array to check whether a line in data.txt file has the right number of fields
	string fieldsCheck[DIM];
	bool valid = true;

	int i = 0;
	int k = 0;
	int badLines = 0;

	//Count the number of data points for allocation
	if (myfile.is_open())
	{
		while (!myfile.eof())
		{
			if(myfile.peek() == EOF)
			{
				break;
			}
			getline(myfile,line);
			i++;
		}
	}

	else
	{
		logger->Log(ERROR, (format("File %1% at line %2%: Unable to open the training data file at %3%")%__LINE__%__FILE__%globalConfig->getPathTrainingFile()).str());
	}

	myfile.close();
	int maxPts = i;

	//Open the file again, allocate the number of points and assign
	myfile.open(inFilePath.data(), ifstream::in);
	dataPts = annAllocPts(maxPts, enabledFeatures); // allocate data points
	normalizedDataPts = annAllocPts(maxPts, enabledFeatures);


	if (myfile.is_open())
	{
		i = 0;

		while (!myfile.eof() && (i < maxPts))
		{
			k = 0;

			if(myfile.peek() == EOF)
			{
				break;
			}

			//initializes fieldsCheck to have all array indices contain the string "NotPresent"
			for(int j = 0; j < DIM; j++)
			{
				fieldsCheck[j] = "NotPresent";
			}

			//this will grab a line of values up to a newline or until DIM values have been taken in.
			while(myfile.peek() != '\n' && k < DIM)
			{
				getline(myfile, fieldsCheck[k], ' ');
				k++;
			}

			//starting from the end of fieldsCheck, if NotPresent is still inside the array, then
			//the line of the data.txt file is incorrect, set valid to false. Note that this
			//only works in regards to the 9 data points preceding the classification,
			//not the classification itself.
			for(int m = DIM - 1; m >= 0 && valid; m--)
			{
				if(!fieldsCheck[m].compare("NotPresent"))
				{
					valid = false;
				}
			}

			//if the next character is a newline after extracting as many data points as possible,
			//then the classification is not present. For now, I will merely discard the line;
			//there may be a more elegant way to do it. (i.e. pass the data to Classify or something)
			if(myfile.peek() == '\n' || myfile.peek() == ' ')
			{
				valid = false;
			}

			//if the line is valid, continue as normal
			if(valid)
			{
				dataPtsWithClass.push_back(new Point(enabledFeatures));

				// Used for matching the 0->DIM index with the 0->enabledFeatures index
				int actualDimension = 0;
				for(int defaultDimension = 0;defaultDimension < DIM;defaultDimension++)
				{
					double temp = strtod(fieldsCheck[defaultDimension].data(), NULL);

					if (featureEnabled[defaultDimension])
					{
						dataPtsWithClass[i]->m_annPoint[actualDimension] = temp;
						dataPts[i][actualDimension] = temp;

						//Set the max values of each feature. (Used later in normalization)
						if(temp > maxFeatureValues[actualDimension])
							maxFeatureValues[actualDimension] = temp;

						meanFeatureValues[actualDimension] += temp;

						actualDimension++;
					}
				}
				getline(myfile,line);
				dataPtsWithClass[i]->m_classification = atoi(line.data());
				i++;
			}
			//but if it isn't, just get to the next line without incrementing i.
			//this way every correct line will be inserted in sequence
			//without any gaps due to perhaps multiple line failures, etc.
			else
			{
				getline(myfile,line);
				badLines++;
			}
		}
		nPts = i;

		for (int j = 0; j < DIM; j++)
			meanFeatureValues[j] /= nPts;
	}
	else
	{
		logger->Log(ERROR, (format("File %1% at line %2%: Unable to open the training data file at %3%")%__LINE__%__FILE__%globalConfig->getPathTrainingFile()).str());
	}
	myfile.close();

	//Normalize the data points

	//Foreach feature within the data point
	for(uint j = 0;j < enabledFeatures;j++)
	{
		//Foreach data point
		for(int i=0;i < nPts;i++)
		{
			normalizedDataPts[i][j] = Normalize(normalization[j], dataPts[i][j], minFeatureValues[j], maxFeatureValues[j]);
		}
	}

	kdTree = new ANNkd_tree(					// build search structure
			normalizedDataPts,					// the data points
					nPts,						// number of points
					enabledFeatures);						// dimension of space
}

double ClassificationEngine::Normalize(normalizationType type, double value, double min, double max)
{
	switch (type)
	{
		case LINEAR:
		{
			return value / max;
		}
		case LINEAR_SHIFT:
		{
			return (value -min) / (max - min);
		}
		case LOGARITHMIC:
		{
			if(!value || !max)
				return 0;
			else return(log(value)/log(max));
			//return (log(value - min + 1)) / (log(max - min + 1));
		}
		case NONORM:
		{
			return value;
		}
		default:
		{
			//logger->Logging(ERROR, "Normalization failed: Normalization type unkown");
			return 0;
		}

		// TODO: A sigmoid normalization function could be very useful,
		// especially if we could somehow use it interactively to set the center and smoothing
		// while looking at the data visualizations to see what works best for a feature
	}
}


void ClassificationEngine::WriteDataPointsToFile(string outFilePath, ANNkd_tree* tree)
{
	ofstream myfile (outFilePath.data());

	if (myfile.is_open())
	{
		for (int i = 0; i < tree->nPoints(); i++ )
		{
			for(int j=0; j < tree->theDim(); j++)
			{
				myfile << tree->thePoints()[i][j] << " ";
			}
			myfile << dataPtsWithClass[i]->m_classification;
			myfile << "\n";
		}
	}
	else
	{
		logger->Log(ERROR, (format("File %1% at line %2%: Unable to open the training data file at %3%")%__LINE__%__FILE__%outFilePath).str());

	}
	myfile.close();

}

