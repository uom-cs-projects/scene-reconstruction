#include <iostream>
#include <numeric>
#include <ctime>
#include <chrono>
#include <unordered_map>
#include <boost/filesystem.hpp>
#include <cvsba/cvsba.h>
#include <GL/glew.h>
#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>
#include <nlohmann/json.hpp>

#include "../include/point2fCompare.hpp"
#include "../include/image_data.hpp"
#include "../include/render_environment.hpp"
#include "../include/render_environment.hpp"
#include "../include/shader.hpp"   

using namespace std;
using namespace boost;
using json = nlohmann::json;

vector<string> acceptedExtensions = {".png", ".jpg", ".PNG", ".JPG"};

int IMAGE_DOWNSCALE_FACTOR = -1, MIN_GUESSES_COUNT = -1, IMAGES_TO_PROCESS = -1;
double FOCAL_LENGTH = -1;
float OPENGL_SCALE_FACTOR = -1.0f;
bool SHOW_MATCHES = false;
string DATASET_DIR = "";
//focal_pixel = (focal_mm / sensor_width_mm) * image_width_in_pixels
//4308 Desk
//9318.70967742 Doll
//3310.4 Dino
//851.01 synthetic1_images
//907.32 Flowerpot

cv::Mat cameraIntrinsic;    //Assume all camera intrinsics are equal for now.
cv::Mat initialPose = cv::Mat::eye(3, 4, CV_64F);

auto oldTime = chrono::steady_clock::now(), newTime = chrono::steady_clock::now();
double deltaT;

vector<ImageData*> images;
map<cv::Point2f, int> previousPairImage2FeaturesToPoints3D; 
//A list of a list of guesses for each 3d Point. Each list gets averaged out to a single 3D point in points3D.
vector<vector<cv::Point3f>> points3DGuesses, points3DColours;
vector<glm::vec3> points3D, pointColours, cameras3D, cameraColours;
//List of a list of each images' detected features. This is not sparse; imagePoints[0][1] does not have to be equal to imagePoints[1][1] even if they do have a match!
vector<vector<cv::Point2f>> imagePoints;    
vector<vector<int>> visibility;  //for each image, is each 3D point represented by a 2D image feature. 1 if yes, 0 if not.
vector<cv::Mat> cameraMatrix;  //The intrinsic matrix for each camera.
vector<cv::Mat> cameraTransforms;
glm::vec3 pointAverage; //  //Center the reconstruction in the scene using this.


void fromCV2GLM(const cv::Mat& cvmat, glm::mat3* glmmat) {
    //Basic conversion method adapted from: https://stackoverflow.com/questions/44409443/how-a-cvmat-translate-from-to-a-glmmat4
    if (cvmat.cols != 3|| cvmat.rows != 3 || cvmat.type() != CV_32FC1) {
        cout << "Matrix conversion error! (3x3)" << endl;
        return;
    }
    memcpy(glm::value_ptr(*glmmat), cvmat.data, 12 * sizeof(float));
}

void fromCV2GLM(const cv::Mat& cvmat, glm::mat4* glmmat) {
    //TODO: This is temp.
    cv::Mat cvmat32;
    cvmat.convertTo(cvmat32, CV_32F);

    //Basic conversion method adapted from: https://stackoverflow.com/questions/44409443/how-a-cvmat-translate-from-to-a-glmmat4
    if (cvmat32.cols != 4|| cvmat32.rows != 4 || cvmat32.type() != CV_32FC1) {
        return;
    }
    memcpy(glm::value_ptr(*glmmat), cvmat32.data, 16 * sizeof(float));
}

void runSBA() {
        // if (imageSets.size() > 1) { //Only run if images > 2
        //     // run sba optimization
        //     try {
        //         sba.run( points3D, imagePoints, visibility, cameraMatrix, cameraRotations, cameraTranslations, distortionCoeffs);
        //     } catch (cv::Exception) {
        //     }

        //         cout << "relative translation after BA: " << imagePair->relativeTranslation << endl;
        //         cout << "image1->worldTranslation after BA" << imagePair->image1->worldTranslation << endl;
        //         cout << "image2->worldTranslation after BA" << imagePair->image2->worldTranslation << endl;
        // }
}

void setupSBA() {
    // cvsba::Sba sba;
    // cv::TermCriteria criteria(CV_TERMCRIT_ITER + CV_TERMCRIT_EPS, 150, 1e-10);
    // cvsba::Sba::Params params;
    // params.iterations = 150;
    // params.type = cvsba::Sba::MOTIONSTRUCTURE;
    // params.minError = 1e-10;
    // params.fixedIntrinsics = 5;
    // params.fixedDistortion = 5;
    // params.verbose = false;
    // sba.setParams(params);
}

void LoadSettings() {
    stringstream settingsPath;
    settingsPath << DATASET_DIR << "/settings.json";

    std::ifstream in(settingsPath.str());
    json settings;
    in >> settings;

    IMAGE_DOWNSCALE_FACTOR = settings["IMAGE_DOWNSCALE_FACTOR"];
    MIN_GUESSES_COUNT = settings["MIN_GUESSES_COUNT"];
    IMAGES_TO_PROCESS = settings["IMAGES_TO_PROCESS"];
    FOCAL_LENGTH = settings["FOCAL_LENGTH"];
    FOCAL_LENGTH /= IMAGE_DOWNSCALE_FACTOR;
    OPENGL_SCALE_FACTOR = settings["OPENGL_SCALE_FACTOR"];
    SHOW_MATCHES = settings["SHOW_MATCHES"];
}

void setupIntrinsicMatrix(int imageWidth, int imageHeight) {
    double cx = imageWidth/2;
    double cy = imageHeight/2;

    cameraIntrinsic = cv::Mat::eye(3, 3, CV_64F);
    cameraIntrinsic.at<double>(0,0) = FOCAL_LENGTH;
    cameraIntrinsic.at<double>(1,1) = FOCAL_LENGTH;
    cameraIntrinsic.at<double>(0,2) = cx;
    cameraIntrinsic.at<double>(1,2) = cy;
}

bool loadImagesAndDetectFeatures() {
    vector<filesystem::path> imagePaths;

    copy_if(filesystem::directory_iterator(DATASET_DIR), filesystem::directory_iterator(), back_inserter(imagePaths), [&](filesystem::path path){
        return find(acceptedExtensions.begin(), acceptedExtensions.end(), path.extension()) != acceptedExtensions.end();
    });
    sort(imagePaths.begin(), imagePaths.end());   //Sort, since directory iteration is not ordered on some file systems

    if (imagePaths.size() == 0) {
        cout << "No Images found at path!" << endl;
        return -1;
    }

    for (vector<filesystem::path>::const_iterator itr = imagePaths.begin(); itr != imagePaths.end(); ++itr) {
        cv::String filePath = cv::String(filesystem::canonical(*itr).string()); //Get full file path, not relative.

        cv::Mat image = cv::imread(filePath, cv::IMREAD_ANYCOLOR);
        cv::resize(image, image, image.size()/IMAGE_DOWNSCALE_FACTOR);

        if (images.size() == 0) {
            setupIntrinsicMatrix(image.size().width, image.size().height);
        }

        ImageData *currentImage = new ImageData(image, cameraIntrinsic,  cv::Mat::eye(3, 4, CV_64F));

        if (images.size() == 0) {   //Setup first image
            currentImage->projectionMatrix = currentImage->cameraIntrinsic  * currentImage->projectionMatrix;
            currentImage->worldTransform = cv::Mat::eye(4, 4, CV_64F);
        }

        images.push_back(currentImage);
    }
    return 1;
}

cv::Mat estimateRelativeTransform(ImageData* image1, ImageData* image2, vector<cv::Point2f> image1Points, vector<cv::Point2f> image2Points) {
    cv::Point2d pp(cv::Mat(image1->cameraIntrinsic).at<double>(0,2), cv::Mat(image1->cameraIntrinsic).at<double>(1,2));
    double focal = cv::Mat(image1->cameraIntrinsic).at<double>(0,0);
    cv::Mat mask;

    cv::Mat essentialMat = cv::findEssentialMat(image2Points, image1Points, focal, pp, cv::RANSAC, 0.999, 1.0, mask);

    //OpenCV returns a non 3x3 matrix if it can't derive an Essential Matrix.
    assert(essentialMat.cols == 3 && essentialMat.rows == 3);

    cv::Mat localTranslation, localRotation;
    cv::recoverPose(essentialMat, image2Points, image1Points, localRotation, localTranslation, focal, pp, mask);

    cv::Mat localTransform = cv::Mat::eye(4, 4, CV_64F);
    localRotation.copyTo(localTransform(cv::Range(0, 3), cv::Range(0, 3)));
    localTranslation.copyTo(localTransform(cv::Range(0, 3), cv::Range(3, 4)));

    return localTransform;
}

void estimateWorldTransform(cv::Mat relativeTransform, int image1Index, int image2Index, 
                                vector<cv::Point2f> image1Points, vector<cv::Point2f> image2Points) {
    images[image2Index]->worldTransform = images[image1Index]->worldTransform * relativeTransform;

    cv::Mat rotationMatrix = images[image2Index]->worldTransform(cv::Range(0, 3), cv::Range(0, 3));
    cv::Mat translationMatrix = images[image2Index]->worldTransform(cv::Range(0, 3), cv::Range(3, 4));
    
    cv::Mat projectionMatrix(3, 4, CV_64F);
    projectionMatrix(cv::Range(0, 3), cv::Range(0, 3)) = rotationMatrix.t(); //The rotation of B with respect to A, rather than A with respect to B.
    projectionMatrix(cv::Range(0, 3), cv::Range(3, 4)) = -rotationMatrix.t() * translationMatrix;
    images[image2Index]->projectionMatrix = images[image2Index]->cameraIntrinsic  * projectionMatrix;
}

vector<cv::Point3f> triangulatePoints(ImageData* image1, ImageData* image2, vector<cv::Point2f> image1Points, vector<cv::Point2f> image2Points) {
    cv::Mat points;
    cv::triangulatePoints(image1->projectionMatrix, image2->projectionMatrix, image1Points, image2Points, points);

    vector<cv::Point3f> points3D;
    for (int i = 0; i < points.cols; i++) {
        vector<cv::Point3f> p3d;
        convertPointsFromHomogeneous(points.col(i).t(), p3d);
        points3D.insert(points3D.end(), p3d.begin(), p3d.end());
    }
    return points3D;
}

void matchFeatures(int image1Index, int image2Index) {
    ImageData* image1 = images[image1Index]; 
    ImageData* image2 = images[image2Index];

    //Feature Match 
        cv::Ptr<cv::DescriptorMatcher> matcher = cv::DescriptorMatcher::create("BruteForce-Hamming");        
        // cv::FlannBasedMatcher matcher = cv::FlannBasedMatcher(cv::makePtr<cv::flann::LshIndexParams>(12, 20, 2));
        // Match features between all images
        vector<cv::Point2f> initialMatchesP1, initialMatchesP2,  filteredMatchesP1, filteredMatchesP2;;
        vector<int> intialIndexesP1, intialIndexesP2, filteredIndexesP1, filteredIndexesP2;
        std::vector<std::vector<cv::DMatch>> knn_matches;
        std::vector<cv::DMatch> initialMatches, filteredMatches;
        matcher->knnMatch((*image1).image_descriptors, (*image2).image_descriptors, knn_matches, 2);

    //Lowes Ratio Test Filter
        const float ratio_thresh = 0.7f;
        for (size_t i = 0; i < knn_matches.size(); i++) {
            if (knn_matches[i].size() > 0 && knn_matches[i][0].distance < ratio_thresh * knn_matches[i][1].distance) {
                initialMatchesP1.push_back(image1->image_keypoints[knn_matches[i][0].queryIdx].pt);
                initialMatchesP2.push_back(image2->image_keypoints[knn_matches[i][0].trainIdx].pt);
                //Store the indexes to avoid the problem of having keypoints not matching up when masked.
                intialIndexesP1.push_back(knn_matches[i][0].queryIdx);
                intialIndexesP2.push_back(knn_matches[i][0].trainIdx);

                initialMatches.push_back(knn_matches[i][0]);
            }
        }

    //Fundamental Matrix Constraint Filter
        vector<uchar> mask;
        cv::findFundamentalMat(initialMatchesP1, initialMatchesP2, cv::FM_RANSAC, 3.0, 0.99, mask);
        
        for (size_t k=0; k < mask.size(); k++) {
            if (mask[k]) {
                filteredIndexesP1.push_back(intialIndexesP1[k]);
                filteredIndexesP2.push_back(intialIndexesP2[k]);

                filteredMatchesP1.push_back(image1->image_keypoints[intialIndexesP1[k]].pt);
                filteredMatchesP2.push_back(image2->image_keypoints[intialIndexesP2[k]].pt);

                filteredMatches.push_back(initialMatches.at(k));
            }
        }

        cv::Mat img_matches;
        cv::drawMatches(image1->image, image1->image_keypoints, image2->image, image2->image_keypoints,
            filteredMatches, img_matches, cv::Scalar::all(-1), cv::Scalar::all(-1),
            vector<char>(), cv::DrawMatchesFlags::DEFAULT );

    // resize(img_matches, img_matches, img_matches.size()/2);
    //Show detected matches in an image viewer for debug purposes. 
        if (SHOW_MATCHES) {
            cv::imshow("Good Matches", img_matches);
            cv::waitKey(0); //Wait for a key to be hit to exit viewer.
        }

        std::time_t result = std::time(nullptr);
        stringstream concat;
        concat << DATASET_DIR << "/results/" << image1Index << ":" << image2Index << ".jpg";
        cv::imwrite(concat.str(), img_matches);

    //Initial estimate for World Position of Image2
        cv::Mat localTransform = estimateRelativeTransform(images[image1Index], images[image2Index], filteredMatchesP1, filteredMatchesP2);
        estimateWorldTransform(localTransform, image1Index, image2Index, filteredMatchesP1, filteredMatchesP2);

    //Triangulate initial guesses for Image2
        vector<cv::Point3f> currentPair3DGuesses = triangulatePoints(image1, image2, filteredMatchesP1, filteredMatchesP2);
        int good_matches = cv::sum(mask)[0];
        assert(good_matches >= 10);

    //Calculate scale factor based on previous points
        cv::Point3f previousPairGuess1, previousPairGuess2, currentPairGuess1, currentPairGuess2; 
        bool firstPair = true;
        double previousPairScale = 0, currentPairScale = 0, scaleFactor = 0;
        int count = 0;
        for (int i = 0; i < currentPair3DGuesses.size(); i++) {
            auto corresponding3DPoint = previousPairImage2FeaturesToPoints3D.find(filteredMatchesP1[i]);
            if (corresponding3DPoint != previousPairImage2FeaturesToPoints3D.end()) {
                if(firstPair) {
                    //The first time we've found a set of three matched points, set them to guess 1 rather than 2.
                    previousPairGuess1 = points3DGuesses[corresponding3DPoint->second].back();
                    firstPair = false;
                    currentPairGuess1 = currentPair3DGuesses[i];
                } else {
                    //We have two points matched across each pair, we can do scaling.
                    previousPairGuess2 = points3DGuesses[corresponding3DPoint->second].back();
                    currentPairGuess2 = currentPair3DGuesses[i];

                    currentPairScale = cv::norm(cv::Mat(currentPairGuess1), cv::Mat(currentPairGuess2));
                    previousPairScale = cv::norm(cv::Mat(previousPairGuess1), cv::Mat(previousPairGuess2));
                    scaleFactor += previousPairScale / currentPairScale;
                    count++;

                    previousPairGuess1 = previousPairGuess2;
                    currentPairGuess1 = currentPairGuess2;
                }
            }
        }

        if (previousPairScale != 0 && currentPairScale != 0) {
            scaleFactor /= count;

            cv::Mat localTranslation = localTransform(cv::Range(0, 3), cv::Range(3, 4));
            localTranslation *= scaleFactor;
            // localTranslation.copyTo(localTransform(cv::Range(0, 3), cv::Range(3, 4)));
            
            estimateWorldTransform(localTransform, image1Index, image2Index, filteredMatchesP1, filteredMatchesP2);

            cameraTransforms.push_back(images[image2Index]->worldTransform);
            
            //Retriangulate Points
                currentPair3DGuesses = triangulatePoints(image1, image2, filteredMatchesP1, filteredMatchesP2);
                int good_matches = cv::sum(mask)[0];
                assert(good_matches >= 10);
        }

        //Push back camera position.
        glm::mat4 i2WorldTransform;
        fromCV2GLM(images[image2Index]->worldTransform, &i2WorldTransform);
        glm::vec4 cameraPos = glm::vec4(1.0f);
        cameras3D.push_back(cameraPos * i2WorldTransform);
        cameraColours.push_back(glm::vec3(1.0f, 1.0f, 1.0f));


    // Put points into final structure.
    map<cv::Point2f, int> currentPairImage2FeaturesToPoints3D;
    for (int i = 0; i < currentPair3DGuesses.size(); i++) {
        auto corresponding3DPoint = previousPairImage2FeaturesToPoints3D.find(filteredMatchesP1[i]);

        cv::Point3f mixedColour;
        //Average colour
        cv::Vec3b col1 = image1->image.at<cv::Vec3b>(filteredMatchesP1[i].y, filteredMatchesP1[i].x),
                    col2 = image2->image.at<cv::Vec3b>(filteredMatchesP2[i].y, filteredMatchesP2[i].x);
        
        mixedColour += cv::Point3f(col1.val[0], col1.val[1], col1.val[2]);
        mixedColour += cv::Point3f(col2.val[0], col2.val[1], col2.val[2]);
        mixedColour /= 2;

        if (corresponding3DPoint != previousPairImage2FeaturesToPoints3D.end()) {
            // int point3DGuessIndex = std::distance(points3DGuesses.begin(), points3DGuesses[corresponding3DPoint->second].back);
            points3DGuesses[corresponding3DPoint->second].push_back(currentPair3DGuesses[i]);
            points3DColours[corresponding3DPoint->second].push_back(mixedColour);

            //Create a binding from the image2 point to the index of the 3D guess list.                
            currentPairImage2FeaturesToPoints3D[filteredMatchesP2[i]] = corresponding3DPoint->second;
        } else { //New point
            //Create a new list of 3D point guesses if we're on a new point.
            vector<cv::Point3f> newGuessList;
            newGuessList.push_back(currentPair3DGuesses[i]);
            points3DGuesses.push_back(newGuessList);

            vector<cv::Point3f> newColourList;
            newColourList.push_back(mixedColour);
            points3DColours.push_back(newColourList);

            //Create a binding from the image2 point to the index of the 3D guess list.
            int new3DGuessindex = points3DGuesses.size()-1;
            currentPairImage2FeaturesToPoints3D[filteredMatchesP2[i]] = new3DGuessindex;
        }   
    }

    previousPairImage2FeaturesToPoints3D = currentPairImage2FeaturesToPoints3D;
    currentPairImage2FeaturesToPoints3D.clear();
}

int main(int argc, const char* argv[]) {
    cout << "Launching Program" << endl;

    DATASET_DIR = filesystem::canonical(argv[1]).string();
	srand (time(NULL));

    LoadSettings();
    setupSBA();

    if (!loadImagesAndDetectFeatures()) return -1;

    //Push back initial camera position.
        cameras3D.push_back(glm::vec3(0.0f));
        cameraColours.push_back(glm::vec3(1.0f, 1.0f, 1.0f));

    //Match features between all images
    for (int i = 0; i < std::min((int) images.size(), IMAGES_TO_PROCESS) - 1; i++) {
        matchFeatures(i, i+1);
    }


    for (int i = 0; i < points3DGuesses.size(); i++) {
        vector<cv::Point3f> currentPointGuesses = points3DGuesses[i], currentPointColours = points3DColours[i];
        cv::Point3f averagedPoint, averagedColour;
        if (currentPointGuesses.size() >= MIN_GUESSES_COUNT) {
            for (int j = 0; j < currentPointGuesses.size(); j++) {
                averagedPoint += currentPointGuesses[j];
                averagedColour += currentPointColours[j];
            }
            averagedPoint /= ((float) currentPointGuesses.size());
            averagedPoint *= OPENGL_SCALE_FACTOR; //Scale up
            averagedColour /= ((float) currentPointColours.size());
            glm::vec3 point = glm::vec3(averagedPoint.x, averagedPoint.y, averagedPoint.z);
            points3D.push_back(point);
            pointAverage += point;
            pointColours.push_back(glm::vec3(averagedColour.z/255.0f, averagedColour.y/255.0f, averagedColour.x/255.0f));
        }
    }

    //Center 3d point cloud in scene
    pointAverage /= points3D.size();
    for (int i = 0; i < points3D.size(); i++) {
        points3D[i] -= pointAverage;
    }

    //Center camera 3d positions in scene
    for (int i = 0; i < cameras3D.size(); i++) {
        cameras3D[i] -= pointAverage;
    }

    renderEnvironment *renderer = new renderEnvironment(0.4f, 0.2f, 0.2f, 0.0f, 0.0f, 0.0f);

    cout << "Initialised renderer" << endl;
        	
    GLuint basicShader = Shader::LoadShaders("./bin/shaders/basic.vertshader", "./bin/shaders/basic.fragshader");
    // renderer->addRenderable(new Renderable(basicShader, cameras3D, cameraColours, GL_POINTS));
	renderer->addRenderable(new Renderable(basicShader, points3D, pointColours, GL_POINTS));

    while (true) {  //TODO: Write proper update & exit logic.
		oldTime = newTime;
    	newTime = chrono::steady_clock::now();
		deltaT = chrono::duration_cast<chrono::milliseconds>(newTime - oldTime).count();

        renderer->update(deltaT);
    }
    return 0;
}