/**
 * Python interface for SuperGlue: https://github.com/magicleap/SuperGluePretrainedNetwork
 */

#include <superglue_pytorch/SuperGlue.h>
#include <rtabmap/utilite/ULogger.h>
#include <rtabmap/utilite/UDirectory.h>
#include <rtabmap/utilite/UFile.h>
#include <rtabmap/utilite/UStl.h>
#include <rtabmap/utilite/UConversion.h>
#include <rtabmap/utilite/UTimer.h>

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#include <numpy/arrayobject.h>

namespace rtabmap
{

class PythonSingleTon
{
public:
	PythonSingleTon() : initialized_(false) {}
	void init() {UScopeMutex lock(mutex_); if(!initialized_)Py_Initialize(); initialized_=true;}
	bool initialized() const {return initialized_;}
	virtual ~PythonSingleTon() {if(initialized_) Py_Finalize();}
private:
	bool initialized_;
	UMutex mutex_;
};

static PythonSingleTon g_python;

SuperGlue::SuperGlue(const std::string & path, float matchThreshold, int iterations, bool cuda) :
		pModule_(0),
		pFunc_(0),
		matchThreshold_(matchThreshold),
		iterations_(iterations),
		cuda_(cuda)
{
	path_ = uReplaceChar(path, '~', UDirectory::homeDir());
	UINFO("path = %s", path_.c_str());

	if(!UFile::exists(path_))
	{
		UERROR("Cannot initialize SuperGlue, the path is not valid: \"%s\"", path_.c_str());
		return;
	}

	if(!g_python.initialized())
	{
		g_python.init();
	}

	std::string superGluePythonDir = UDirectory::getDir(path_);
	if(!superGluePythonDir.empty())
	{
		PyRun_SimpleString("import sys");
		PyRun_SimpleString(uFormat("sys.path.append(\"%s\")", superGluePythonDir.c_str()).c_str());
	}

	_import_array();

	std::string scriptName = uSplit(UFile::getName(path_), '.').front();
	PyObject * pName = PyUnicode_FromString(scriptName.c_str());
	pModule_ = PyImport_Import(pName);
	Py_DECREF(pName);

	if(!pModule_)
	{
		UERROR("Module %s could not be imported!", scriptName.c_str());
	}
}

SuperGlue::~SuperGlue()
{
	if(pFunc_)
	{
		Py_DECREF(pFunc_);
	}
	if(pModule_)
	{
		Py_DECREF(pModule_);
	}
}

std::vector<cv::DMatch> SuperGlue::match(
		  const cv::Mat & descriptorsQuery,
		  const cv::Mat & descriptorsTrain,
		  const std::vector<cv::KeyPoint> & keypointsQuery,
		  const std::vector<cv::KeyPoint> & keypointsTrain,
		  const cv::Size & imageSize)
{
	UTimer timer;
	std::vector<cv::DMatch> matches;

	if(!pModule_)
	{
		UERROR("SuperGlue python module not loaded!");
		return matches;
	}

	if(descriptorsQuery.cols == 256 && // Only SuperPoint is supported!
	   descriptorsQuery.cols == descriptorsTrain.cols &&
	   descriptorsQuery.type() == CV_32F &&
	   descriptorsTrain.type() == CV_32F &&
	   descriptorsQuery.rows == (int)keypointsQuery.size() &&
	   descriptorsTrain.rows == (int)keypointsTrain.size() &&
	   imageSize.width>0 && imageSize.height>0)
	{

		UDEBUG("matchThreshold=%f, iterations=%d, cuda=%d", matchThreshold_, iterations_, cuda_?1:0);

		if(!pFunc_)
		{
			PyObject * pFunc = PyObject_GetAttrString(pModule_, "init");
			if(pFunc)
			{
				if(PyCallable_Check(pFunc))
				{
					PyObject_CallFunction(pFunc, "ifii", descriptorsQuery.cols, matchThreshold_, iterations_, cuda_?1:0);

					pFunc_ = PyObject_GetAttrString(pModule_, "match");
					if(pFunc_ && PyCallable_Check(pFunc_))
					{
						// we are ready!
					}
					else
					{
						UERROR("Cannot find method \"match(...)\" in %s", path_.c_str());
						if(pFunc_)
						{
							Py_DECREF(pFunc_);
							pFunc_ = 0;
						}
						return matches;
					}
				}
				else
				{
					UERROR("Cannot call method \"init(...)\" in %s", path_.c_str());
					return matches;
				}
				Py_DECREF(pFunc);
			}
			else
			{
				UERROR("Cannot find method \"init(...)\"");
				return matches;
			}
			UDEBUG("init time = %fs", timer.ticks());
		}

		if(pFunc_)
		{
			std::vector<float> descriptorsQueryV(descriptorsQuery.rows * descriptorsQuery.cols);
			memcpy(descriptorsQueryV.data(), descriptorsQuery.data, descriptorsQuery.total()*sizeof(float));
			npy_intp dimsFrom[2] = {descriptorsQuery.rows, descriptorsQuery.cols};
			PyObject* pDescriptorsQuery = PyArray_SimpleNewFromData(2, dimsFrom, NPY_FLOAT, (void*)descriptorsQueryV.data());
			UASSERT(pDescriptorsQuery);

			npy_intp dimsTo[2] = {descriptorsTrain.rows, descriptorsTrain.cols};
			std::vector<float> descriptorsTrainV(descriptorsTrain.rows * descriptorsTrain.cols);
			memcpy(descriptorsTrainV.data(), descriptorsTrain.data, descriptorsTrain.total()*sizeof(float));
			PyObject* pDescriptorsTrain = PyArray_SimpleNewFromData(2, dimsTo, NPY_FLOAT, (void*)descriptorsTrainV.data());
			UASSERT(pDescriptorsTrain);

			std::vector<float> keypointsQueryV(keypointsQuery.size()*2);
			std::vector<float> scoresQuery(keypointsQuery.size());
			for(size_t i=0; i<keypointsQuery.size(); ++i)
			{
				keypointsQueryV[i*2] = keypointsQuery[i].pt.x;
				keypointsQueryV[i*2+1] = keypointsQuery[i].pt.y;
				scoresQuery[i] = keypointsQuery[i].response;
			}

			std::vector<float> keypointsTrainV(keypointsTrain.size()*2);
			std::vector<float> scoresTrain(keypointsTrain.size());
			for(size_t i=0; i<keypointsTrain.size(); ++i)
			{
				keypointsTrainV[i*2] = keypointsTrain[i].pt.x;
				keypointsTrainV[i*2+1] = keypointsTrain[i].pt.y;
				scoresTrain[i] = keypointsTrain[i].response;
			}

			npy_intp dimsKpQuery[2] = {(int)keypointsQuery.size(), 2};
			PyObject* pKeypointsQuery = PyArray_SimpleNewFromData(2, dimsKpQuery, NPY_FLOAT, (void*)keypointsQueryV.data());
			UASSERT(pKeypointsQuery);

			npy_intp dimsKpTrain[2] = {(int)keypointsTrain.size(), 2};
			PyObject* pkeypointsTrain = PyArray_SimpleNewFromData(2, dimsKpTrain, NPY_FLOAT, (void*)keypointsTrainV.data());
			UASSERT(pkeypointsTrain);

			npy_intp dimsScoresQuery[1] = {(int)keypointsQuery.size()};
			PyObject* pScoresQuery = PyArray_SimpleNewFromData(1, dimsScoresQuery, NPY_FLOAT, (void*)scoresQuery.data());
			UASSERT(pScoresQuery);

			npy_intp dimsScoresTrain[1] = {(int)keypointsTrain.size()};
			PyObject* pScoresTrain = PyArray_SimpleNewFromData(1, dimsScoresTrain, NPY_FLOAT, (void*)scoresTrain.data());
			UASSERT(pScoresTrain);

			PyObject * pImageWidth = PyLong_FromLong(imageSize.width);
			PyObject * pImageHeight = PyLong_FromLong(imageSize.height);

			UDEBUG("Preparing data time = %fs", timer.ticks());

			PyObject *pReturn = PyObject_CallFunctionObjArgs(pFunc_, pKeypointsQuery, pkeypointsTrain, pScoresQuery, pScoresTrain, pDescriptorsQuery, pDescriptorsTrain, pImageWidth, pImageHeight, NULL);
			UASSERT(pReturn);

			UDEBUG("Python matching time = %fs", timer.ticks());

			PyArrayObject *np_ret = reinterpret_cast<PyArrayObject*>(pReturn);

			// Convert back to C++ array and print.
			int len1 = PyArray_SHAPE(np_ret)[0];
			int len2 = PyArray_SHAPE(np_ret)[1];
			//int type = PyArray_TYPE(np_ret); // Should be long
			long* c_out = reinterpret_cast<long*>(PyArray_DATA(np_ret));
			for (int i = 0; i < len1*len2; i+=2)
			{
				matches.push_back(cv::DMatch(c_out[i], c_out[i+1], 0));
			}

			Py_DECREF(pReturn);
			Py_DECREF(pDescriptorsQuery);
			Py_DECREF(pDescriptorsTrain);
			Py_DECREF(pKeypointsQuery);
			Py_DECREF(pkeypointsTrain);
			Py_DECREF(pScoresQuery);
			Py_DECREF(pScoresTrain);
			Py_DECREF(pImageWidth);
			Py_DECREF(pImageHeight);

			UDEBUG("Fill matches (%d/%d) and cleanup time = %fs", matches.size(), std::min(descriptorsQuery.rows, descriptorsTrain.rows), timer.ticks());
		}
	}
	else if(descriptorsQuery.cols != 256)
	{
		UERROR("Only descriptor size of 256 (SuperPoint) is "
			   "supported with SuperGlue! Current descriptor size=%d.",
				descriptorsQuery.cols);
	}
	else
	{
		UERROR("Invalid inputs! SuperGlue requires SuperPoint descriptors (dim=256).");
	}
	return matches;
}

}
